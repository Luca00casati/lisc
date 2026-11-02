// main.cpp — the lisc driver: source → LLVM IR → optimised native object →
// executable (linked by the system C compiler, against libc and libm).
//
//   lisc prog.lsp [more.c|.o ...] [-o prog] [-O0..-O3] [-g] [-w] [--checks] [--time] [-I dir] [-D NAME[=value]] [--emit-llvm] [-c] [-lname ...]
//
// Other input files go to the linker (cc), so C code can be linked in.
// import-c searches the source file's directory and -I directories for headers.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <chrono>

#include <sys/wait.h>
#include <unistd.h>

#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/StandardInstrumentations.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>

#include "compiler.h"

namespace {

// --time: how long each phase took
bool timing = false;
auto t_last = std::chrono::steady_clock::now();
void phase(const char *name) {
    auto now = std::chrono::steady_clock::now();
    if (timing) fprintf(stderr, "lisc: %-12s %7.3f s\n", name, std::chrono::duration<double>(now - t_last).count());
    t_last = now;
}

[[noreturn]] void usage() {
    fprintf(stderr, "usage: lisc file.lsp [file.c|file.o ...] [-o out] [-O0..-O3] [-g] [-w] [--checks] [--time] [-I dir] [-D NAME[=value]] [--emit-llvm] [-c] [-lname ...]\n");
    exit(1);
}

int run(const std::vector<std::string> &argv) {
    std::vector<char *> a;
    for (auto &s : argv) a.push_back((char *)s.c_str());
    a.push_back(nullptr);
    pid_t pid = fork();
    if (pid == 0) {
        execvp(a[0], a.data());
        perror(a[0]);
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

void optimize(llvm::Module &m, llvm::TargetMachine *tm, int level) {
    if (level == 0) return;
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;
    // the standard instrumentation is what makes passes skip optnone functions
    llvm::PassInstrumentationCallbacks pic;
    llvm::StandardInstrumentations si(m.getContext(), false);
    si.registerCallbacks(pic, &mam);
    llvm::PassBuilder pb(tm, llvm::PipelineTuningOptions(), std::nullopt, &pic);
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
    const llvm::OptimizationLevel levels[] = {llvm::OptimizationLevel::O0, llvm::OptimizationLevel::O1,
                                              llvm::OptimizationLevel::O2, llvm::OptimizationLevel::O3};
    llvm::ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(levels[level]);
    mpm.run(m, mam);
}

}  // namespace

int main(int argc, char **argv) {
    const char *in = nullptr;
    std::string out;
    int opt = 2;
    bool emit_llvm = false, object_only = false, debug = false, checks = false;
    std::vector<std::string> libs, includes, defines;
    for (int i = 1; i < argc; i++) {
        const char *s = argv[i];
        size_t len = strlen(s);
        if (!strcmp(s, "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(s, "-I") && i + 1 < argc) includes.push_back(argv[++i]);
        else if (!strncmp(s, "-D", 2) && (s[2] || i + 1 < argc)) defines.push_back(s[2] ? s + 2 : argv[++i]);
        else if (!strncmp(s, "-I", 2) && s[2]) includes.push_back(s + 2);
        else if (s[0] != '-' && !(len > 4 && !strcmp(s + len - 4, ".lsp"))) libs.push_back(s);
        else if (s[0] == '-' && s[1] == 'O' && s[2] >= '0' && s[2] <= '3' && !s[3]) opt = s[2] - '0';
        else if (!strcmp(s, "--emit-llvm")) emit_llvm = true;
        else if (!strcmp(s, "-g")) debug = true;
        else if (!strcmp(s, "-w")) warnings = false;
        else if (!strcmp(s, "--checks")) checks = true;
        else if (!strcmp(s, "--time")) timing = true;
        else if (!strcmp(s, "-c")) object_only = true;
        else if (!strncmp(s, "-l", 2) && s[2]) libs.push_back(s);
        else if (s[0] == '-' || in) usage();
        else in = s;
    }
    if (!in) usage();
    if (out.empty()) {
        out = in;
        size_t dot = out.rfind('.');
        if (dot != std::string::npos) out.resize(dot);
        if (emit_llvm) out += ".ll";
        else if (object_only) out += ".o";
    }

    Compiler c;
    std::string dir = in;
    dir = dir.find('/') == std::string::npos ? "." : dir.substr(0, dir.rfind('/'));
    c.include_dirs.push_back(dir);
    c.include_dirs.insert(c.include_dirs.end(), includes.begin(), includes.end());
    char self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n > 0) {
        std::string exe(self, n);
        c.lib_dir = exe.substr(0, exe.rfind('/')) + "/lib";
    }
    for (auto &d : defines) {
        size_t eq = d.find('=');
        c.define_const(d.substr(0, eq), eq == std::string::npos ? "1" : d.substr(eq + 1));
    }
    c.debug = debug;
    c.checks = checks;
    phase("start");
    c.compile(read_file(in));
    phase("front end");
    llvm::Module &m = c.module();

    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    std::string triple = llvm::sys::getDefaultTargetTriple(), err;
    const llvm::Target *target = llvm::TargetRegistry::lookupTarget(triple, err);
    if (!target) { fprintf(stderr, "lisc: %s\n", err.c_str()); return 1; }
    llvm::StringMap<bool> host;
    std::string features;
    if (llvm::sys::getHostCPUFeatures(host))
        for (auto &f : host) features += (features.empty() ? "" : ",") + std::string(f.second ? "+" : "-") + f.first().str();
    llvm::TargetMachine *tm = target->createTargetMachine(triple, llvm::sys::getHostCPUName(), features, llvm::TargetOptions(),
                                                          llvm::Reloc::PIC_, std::nullopt,
                                                          opt == 0 ? llvm::CodeGenOptLevel::None : opt == 1 ? llvm::CodeGenOptLevel::Less
                                                          : opt == 2 ? llvm::CodeGenOptLevel::Default : llvm::CodeGenOptLevel::Aggressive);
    m.setDataLayout(tm->createDataLayout());
    m.setTargetTriple(triple);
    phase("setup");
    optimize(m, tm, opt);
    phase("optimise");

    std::error_code ec;
    if (emit_llvm) {
        llvm::raw_fd_ostream os(out, ec, llvm::sys::fs::OF_Text);
        if (ec) { fprintf(stderr, "lisc: %s: %s\n", out.c_str(), ec.message().c_str()); return 1; }
        m.print(os, nullptr);
        return 0;
    }
    std::string obj = object_only ? out : out + ".o";
    {
        llvm::raw_fd_ostream os(obj, ec, llvm::sys::fs::OF_None);
        if (ec) { fprintf(stderr, "lisc: %s: %s\n", obj.c_str(), ec.message().c_str()); return 1; }
        llvm::legacy::PassManager pm;
        if (tm->addPassesToEmitFile(pm, os, nullptr, llvm::CodeGenFileType::ObjectFile)) {
            fprintf(stderr, "lisc: the target cannot emit object files\n");
            return 1;
        }
        pm.run(m);
        phase("codegen");
    }
    if (object_only) return 0;
    std::vector<std::string> link = {"cc", "-o", out, obj, "-lm"};
    link.insert(link.end(), libs.begin(), libs.end());
    int r = run(link);
    phase("link");
    unlink(obj.c_str());
    return r;
}
