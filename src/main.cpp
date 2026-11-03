// main.cpp — the lisc driver: source → LLVM IR → optimised native object →
// executable (linked by the system C compiler, against libc and libm).
//
//   lisc prog.lsp [more.c|.o ...] [-o prog] [-O0..-O3] [-g] [-w] [--checks] [--time] [--no-cache] [-I dir] [-D NAME[=value]] [--emit-llvm] [-c] [-lname ...]
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

#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
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

#include "build.h"
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
    fprintf(stderr, "usage: lisc file.lsp [file.c|file.o ...] [-o out] [-O0..-O3] [-g] [-w] [--checks] [--time] [--no-cache] [-I dir] [-D NAME[=value]] [--emit-llvm] [-c] [-lname ...]\n");
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

}  // namespace

int fmt_main(int argc, char **argv);  // fmt.cpp

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "fmt")) return fmt_main(argc, argv);
    const char *in = nullptr;
    std::string out;
    int opt = 2;
    bool emit_llvm = false, object_only = false, debug = false, checks = false, c_cache = true;
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
        else if (!strcmp(s, "--no-cache")) c_cache = false;
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
    c.c_cache = c_cache;
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
    Target t{target, triple, llvm::sys::getHostCPUName().str(), features};
    llvm::TargetMachine *tm = t.machine(opt);
    m.setDataLayout(tm->createDataLayout());
    m.setTargetTriple(triple);
    phase("setup");

    std::vector<std::string> objects, temps;
    if (emit_llvm || object_only) {  // one module, one file
        optimize(m, tm, opt);
        lower_aggregate_copies(m);
        phase("optimise");
        std::error_code ec;
        if (emit_llvm) {
            llvm::raw_fd_ostream os(out, ec, llvm::sys::fs::OF_Text);
            if (ec) { fprintf(stderr, "lisc: %s: %s\n", out.c_str(), ec.message().c_str()); return 1; }
            m.print(os, nullptr);
            return 0;
        }
        bool ok = emit_object(m, tm, out);
        phase("codegen");
        return ok ? 0 : 1;
    }
    // a part per lisc module, compiled in parallel, from the object cache when unchanged
    BuildStats stats;
    if (!build_parts(m, t, opt, c_cache, out + ".part", objects, temps, stats)) return 1;
    if (timing) fprintf(stderr, "lisc: %zu parts, %zu compiled, %zu from the cache\n", stats.parts, stats.compiled, stats.parts - stats.compiled);
    phase("compile");
    std::vector<std::string> link = {"cc", "-o", out};
    link.insert(link.end(), objects.begin(), objects.end());
    link.push_back("-lm");
    link.insert(link.end(), libs.begin(), libs.end());
    int r = run(link);
    phase("link");
    for (auto &f : temps) unlink(f.c_str());
    return r;
}
