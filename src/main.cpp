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

// Struct and array values copied whole — a load of one straight into a store,
// or a zero stored — become memcpy / memset. The optimiser does that itself,
// but not in optnone functions (and not at -O0); there, the code generator's
// fast instruction selector can't handle whole-aggregate loads and stores and
// falls back to the slow one for the whole function.
void lower_aggregate_copies(llvm::Module &m) {
    const llvm::DataLayout &dl = m.getDataLayout();
    for (llvm::Function &f : m) {
        if (f.isDeclaration()) continue;
        std::vector<llvm::StoreInst *> zeros;
        std::vector<llvm::LoadInst *> loads;
        for (llvm::BasicBlock &bb : f)
            for (llvm::Instruction &i : bb) {
                if (auto *st = llvm::dyn_cast<llvm::StoreInst>(&i)) {
                    if (llvm::isa<llvm::ConstantAggregateZero>(st->getValueOperand()) && !st->isVolatile()) zeros.push_back(st);
                } else if (auto *ld = llvm::dyn_cast<llvm::LoadInst>(&i)) {
                    if (!ld->getType()->isAggregateType() || ld->isVolatile() || ld->use_empty()) continue;
                    bool only_stored = true;  // (used only as the value of stores)
                    for (llvm::User *u : ld->users()) {
                        auto *st = llvm::dyn_cast<llvm::StoreInst>(u);
                        only_stored &= st && st->getValueOperand() == ld && st->getPointerOperand() != ld && !st->isVolatile();
                    }
                    if (only_stored) loads.push_back(ld);
                }
            }
        for (llvm::StoreInst *st : zeros) {
            llvm::IRBuilder<> b(st);
            b.CreateMemSet(st->getPointerOperand(), b.getInt8(0), dl.getTypeStoreSize(st->getValueOperand()->getType()), st->getAlign());
            st->eraseFromParent();
        }
        for (llvm::LoadInst *ld : loads) {
            uint64_t size = dl.getTypeStoreSize(ld->getType());
            llvm::Value *src = ld->getPointerOperand();
            llvm::MaybeAlign src_align = ld->getAlign();
            // one store right after, nothing written between: copy straight from the source
            auto *only = ld->hasOneUse() ? llvm::cast<llvm::StoreInst>(*ld->user_begin()) : nullptr;
            bool direct = only && only->getParent() == ld->getParent();
            for (llvm::Instruction *i = ld->getNextNode(); direct && i != only; i = i->getNextNode()) direct = !i->mayWriteToMemory();
            if (!direct) {  // otherwise a copy taken where the load was
                llvm::IRBuilder<> eb(&f.getEntryBlock(), f.getEntryBlock().begin());
                llvm::AllocaInst *tmp = eb.CreateAlloca(ld->getType());
                tmp->setAlignment(ld->getAlign());
                llvm::IRBuilder<> b(ld);
                b.CreateMemCpy(tmp, ld->getAlign(), src, ld->getAlign(), size);
                src = tmp;
            }
            std::vector<llvm::StoreInst *> sts;
            for (llvm::User *u : ld->users()) sts.push_back(llvm::cast<llvm::StoreInst>(u));
            for (llvm::StoreInst *st : sts) {
                llvm::IRBuilder<> b(st);
                b.CreateMemCpy(st->getPointerOperand(), st->getAlign(), src, src_align, size);
                st->eraseFromParent();
            }
            ld->eraseFromParent();
        }
    }
}

// The optnone functions, moved to a module of their own (returned; null if
// there are none), to be compiled as at -O0: the fast instruction selector
// and register allocator, which is what optnone asks for anyway — in the
// optimised module the code generator gives them its full (slow) treatment.
// Names the two modules share become hidden externals; constants are copied.
std::unique_ptr<llvm::Module> split_optnone(llvm::Module &m) {
    bool any = false;
    for (llvm::Function &f : m) any |= !f.isDeclaration() && f.hasFnAttribute(llvm::Attribute::OptimizeNone);
    if (!any) return nullptr;
    auto copied = [](const llvm::GlobalValue *g) {  // a constant both modules get a copy of
        auto *v = llvm::dyn_cast<llvm::GlobalVariable>(g);
        return v && v->isConstant() && v->hasLocalLinkage();
    };
    int n = 0;
    for (llvm::GlobalValue &g : m.global_values()) {
        if (g.isDeclaration() || !g.hasLocalLinkage() || copied(&g)) continue;
        if (!g.hasName()) g.setName("lisc.anon." + std::to_string(n++));
        g.setLinkage(llvm::GlobalValue::ExternalLinkage);
        g.setVisibility(llvm::GlobalValue::HiddenVisibility);
        g.setDSOLocal(true);
    }
    llvm::ValueToValueMapTy vmap;
    std::unique_ptr<llvm::Module> m0 = llvm::CloneModule(m, vmap, [&](const llvm::GlobalValue *g) {
        auto *f = llvm::dyn_cast<llvm::Function>(g);
        return (f && f->hasFnAttribute(llvm::Attribute::OptimizeNone)) || copied(g);
    });
    for (llvm::GlobalValue &g : m0->global_values())
        if (g.isDeclaration() && !g.hasDefaultVisibility()) g.setDSOLocal(true);
    for (llvm::Function &f : m)
        if (!f.isDeclaration() && f.hasFnAttribute(llvm::Attribute::OptimizeNone)) {
            f.deleteBody();
            f.setDSOLocal(true);
        }
    return m0;
}

bool emit_object(llvm::Module &m, llvm::TargetMachine *tm, const std::string &path) {
    std::error_code ec;
    llvm::raw_fd_ostream os(path, ec, llvm::sys::fs::OF_None);
    if (ec) { fprintf(stderr, "lisc: %s: %s\n", path.c_str(), ec.message().c_str()); return false; }
    llvm::legacy::PassManager pm;
    if (tm->addPassesToEmitFile(pm, os, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        fprintf(stderr, "lisc: the target cannot emit object files\n");
        return false;
    }
    pm.run(m);
    return true;
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
    auto machine = [&](llvm::CodeGenOptLevel level) {
        return target->createTargetMachine(triple, llvm::sys::getHostCPUName(), features, llvm::TargetOptions(), llvm::Reloc::PIC_,
                                           std::nullopt, level);
    };
    llvm::TargetMachine *tm = machine(opt == 0 ? llvm::CodeGenOptLevel::None : opt == 1 ? llvm::CodeGenOptLevel::Less
                                      : opt == 2 ? llvm::CodeGenOptLevel::Default : llvm::CodeGenOptLevel::Aggressive);
    m.setDataLayout(tm->createDataLayout());
    m.setTargetTriple(triple);
    phase("setup");
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
    std::string obj = object_only ? out : out + ".o", obj0 = out + ".optnone.o";
    std::unique_ptr<llvm::Module> m0 = opt > 0 && !object_only ? split_optnone(m) : nullptr;
    if (!emit_object(m, tm, obj)) return 1;
    phase("codegen");
    if (m0) {
        if (!emit_object(*m0, machine(llvm::CodeGenOptLevel::None), obj0)) return 1;
        phase("codegen optnone");
    }
    if (object_only) return 0;
    std::vector<std::string> link = {"cc", "-o", out, obj, "-lm"};
    if (m0) link.push_back(obj0);
    link.insert(link.end(), libs.begin(), libs.end());
    int r = run(link);
    phase("link");
    unlink(obj.c_str());
    if (m0) unlink(obj0.c_str());
    return r;
}
