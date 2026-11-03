// build.cpp — from the program's LLVM module to object files: optimised and
// compiled in parallel, one part per lisc module, each part's object cached.
//
// The front end makes one module for the whole program. It is split into
// parts, one per lisc module (a function or global belongs to the module its
// name starts with: kir::emit, vec::push(i32)), plus one per module for its
// :optnone functions, which are compiled as at -O0. Each part is optimised and
// compiled on its own thread, in its own LLVM context.
//
// Small functions a part calls in another part are copied into it as
// available_externally: the optimiser can inline them, and they aren't
// compiled there. Names used across parts become hidden externals; read-only
// constants (string literals) are copied into each part that uses them.
//
// A part's object is kept in ~/.cache/lisc/obj, named after a hash of the
// part's exact bitcode and of everything else that shapes the machine code
// (level, CPU, lisc itself). A part whose code didn't change — including the
// bodies it inlines — is not compiled again.

#include "build.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <set>
#include <thread>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/StandardInstrumentations.h>
#include <llvm/Support/BLAKE3.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Transforms/Utils/Cloning.h>

std::string lisc_cache_dir();  // cimport.cpp

llvm::TargetMachine *Target::machine(int level) const {
    llvm::CodeGenOptLevel l = level == 0   ? llvm::CodeGenOptLevel::None
                              : level == 1 ? llvm::CodeGenOptLevel::Less
                              : level == 2 ? llvm::CodeGenOptLevel::Default
                                           : llvm::CodeGenOptLevel::Aggressive;
    return target->createTargetMachine(triple, cpu, features, llvm::TargetOptions(), llvm::Reloc::PIC_, std::nullopt, l);
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

namespace {

const unsigned INLINE_LIMIT = 120;  // instructions: a function this small is copied where it is called

bool optnone(const llvm::Function &f) { return f.hasFnAttribute(llvm::Attribute::OptimizeNone); }

// a constant each part using it gets a copy of (string literals and such)
bool copied(const llvm::GlobalValue *g) {
    auto *v = llvm::dyn_cast<llvm::GlobalVariable>(g);
    return v && v->isConstant() && v->hasLocalLinkage() && v->hasInitializer();
}

// the part a definition belongs to: its lisc module ("" for names without one)
std::string part_of(const llvm::GlobalValue &g) {
    std::string n = g.getName().str();
    size_t p = n.find("::");
    std::string base = p == std::string::npos ? "" : n.substr(0, p);
    if (auto *f = llvm::dyn_cast<llvm::Function>(&g); f && optnone(*f)) base += "#optnone";
    return base;
}

// the globals a constant refers to (through copied constants too)
void const_refs(const llvm::Constant *c, std::set<const llvm::GlobalValue *> &out, std::set<const llvm::Constant *> &seen) {
    if (!seen.insert(c).second) return;
    if (auto *g = llvm::dyn_cast<llvm::GlobalValue>(c)) {
        out.insert(g);
        if (copied(g)) const_refs(llvm::cast<llvm::GlobalVariable>(g)->getInitializer(), out, seen);
        return;
    }
    for (const llvm::Use &u : c->operands())
        if (auto *k = llvm::dyn_cast<llvm::Constant>(u.get())) const_refs(k, out, seen);
}

std::set<const llvm::GlobalValue *> refs_of(const llvm::GlobalValue &g) {
    std::set<const llvm::GlobalValue *> out;
    std::set<const llvm::Constant *> seen;
    if (auto *f = llvm::dyn_cast<llvm::Function>(&g)) {
        for (const llvm::BasicBlock &bb : *f)
            for (const llvm::Instruction &i : bb)
                for (const llvm::Use &u : i.operands())
                    if (auto *k = llvm::dyn_cast<llvm::Constant>(u.get())) const_refs(k, out, seen);
    } else if (auto *v = llvm::dyn_cast<llvm::GlobalVariable>(&g); v && v->hasInitializer()) {
        const_refs(v->getInitializer(), out, seen);
    }
    return out;
}

unsigned size_of_fn(const llvm::Function &f) {
    unsigned n = 0;
    for (const llvm::BasicBlock &bb : f) n += bb.size();
    return n;
}

std::string hex(const uint8_t *p, size_t n) {
    static const char *d = "0123456789abcdef";
    std::string r;
    for (size_t i = 0; i < n; i++) r += d[p[i] >> 4], r += d[p[i] & 15];
    return r;
}

// what, besides a part's bitcode, decides its machine code
std::string self_id() {
    struct stat sb;
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    std::string path = n > 0 ? std::string(exe, n) : "?";
    long long mt = 0, size = 0;
    if (stat(path.c_str(), &sb) == 0) mt = (long long)sb.st_mtim.tv_sec * 1000000000LL + sb.st_mtim.tv_nsec, size = sb.st_size;
    return "lisc-obj v1 " + path + " " + std::to_string(mt) + " " + std::to_string(size);
}

// objects nobody used for a month go
void prune(const std::string &dir) {
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    time_t old = time(nullptr) - 30 * 24 * 3600;
    while (dirent *e = readdir(d)) {
        std::string p = dir + "/" + e->d_name;
        struct stat sb;
        size_t len = strlen(e->d_name);
        if (len > 2 && !strcmp(e->d_name + len - 2, ".o") && stat(p.c_str(), &sb) == 0 && sb.st_mtime < old) unlink(p.c_str());
    }
    closedir(d);
}

}  // namespace

bool build_parts(llvm::Module &m, const Target &t, int level, bool use_cache, const std::string &tmp_base,
                 std::vector<std::string> &objects, std::vector<std::string> &temps, BuildStats &stats) {
    // the parts, and what each definition refers to
    std::map<std::string, std::vector<llvm::GlobalValue *>> parts;
    std::map<const llvm::GlobalValue *, std::string> part;
    std::map<const llvm::GlobalValue *, std::set<const llvm::GlobalValue *>> refs;
    for (llvm::GlobalValue &g : m.global_values()) {
        if (g.isDeclaration() || copied(&g)) continue;
        part[&g] = part_of(g);
        parts[part[&g]].push_back(&g);
        refs[&g] = refs_of(g);
    }
    // the small functions of other parts each part calls: copied in, for inlining
    auto copyable = [&](const llvm::GlobalValue *g) {
        auto *f = llvm::dyn_cast<llvm::Function>(g);
        return f && !f->isDeclaration() && !optnone(*f) && !f->isVarArg() && f->getName() != "main" && level > 0 &&
               size_of_fn(*f) <= INLINE_LIMIT;
    };
    std::map<std::string, std::set<const llvm::GlobalValue *>> avail;
    std::set<const llvm::GlobalValue *> shared;  // used from a part other than its own
    for (auto &p : parts) {
        std::set<const llvm::GlobalValue *> users(p.second.begin(), p.second.end());
        for (const llvm::GlobalValue *g : p.second)
            for (const llvm::GlobalValue *r : refs[g])
                if (part.count(r) && part[r] != p.first && copyable(r)) avail[p.first].insert(r);
        users.insert(avail[p.first].begin(), avail[p.first].end());
        for (const llvm::GlobalValue *g : users)
            for (const llvm::GlobalValue *r : refs[g])
                if (part.count(r) && part[r] != p.first) shared.insert(r);
        for (const llvm::GlobalValue *g : avail[p.first]) shared.insert(g);
    }
    int anon = 0;
    for (const llvm::GlobalValue *cg : shared) {
        auto *g = const_cast<llvm::GlobalValue *>(cg);
        if (!g->hasLocalLinkage()) continue;
        if (!g->hasName()) g->setName("lisc.anon." + std::to_string(anon++));
        g->setLinkage(llvm::GlobalValue::ExternalLinkage);
        g->setVisibility(llvm::GlobalValue::HiddenVisibility);
        g->setDSOLocal(true);
    }

    // each part as bitcode, and its object's name
    struct Job {
        std::string name, bitcode, object;
        int level;
        bool cached;
    };
    std::vector<Job> jobs;
    std::string dir = use_cache ? lisc_cache_dir() : "";
    if (!dir.empty()) {
        dir += "/obj";
        llvm::sys::fs::create_directories(dir);
    }
    std::string id = self_id() + " " + t.triple + " " + t.cpu + " " + t.features;
    for (auto &p : parts) {
        const std::set<const llvm::GlobalValue *> &av = avail[p.first];
        llvm::ValueToValueMapTy vmap;
        std::unique_ptr<llvm::Module> pm = llvm::CloneModule(m, vmap, [&](const llvm::GlobalValue *g) {
            return copied(g) || av.count(g) || (part.count(g) && part.at(g) == p.first);
        });
        for (const llvm::GlobalValue *g : av)
            if (auto *f = llvm::dyn_cast<llvm::Function>(vmap[g])) f->setLinkage(llvm::GlobalValue::AvailableExternallyLinkage);
        // what this part doesn't use goes (the other parts' constants and declarations)
        for (bool again = true; again;) {
            again = false;
            for (auto it = pm->global_begin(); it != pm->global_end();) {
                llvm::GlobalVariable &g = *it++;
                g.removeDeadConstantUsers();
                if (g.use_empty() && (g.isDeclaration() || copied(&g))) { g.eraseFromParent(); again = true; }
            }
            for (auto it = pm->begin(); it != pm->end();) {
                llvm::Function &f = *it++;
                f.removeDeadConstantUsers();
                if (f.use_empty() && (f.isDeclaration() || f.hasAvailableExternallyLinkage()) && !f.isIntrinsic()) { f.eraseFromParent(); again = true; }
            }
        }
        for (llvm::GlobalValue &g : pm->global_values())
            if (g.isDeclaration() && !g.hasDefaultVisibility()) g.setDSOLocal(true);
        Job j{p.first.empty() ? "(program)" : p.first, "", "", p.first.find("#optnone") != std::string::npos ? 0 : level, false};
        llvm::raw_string_ostream os(j.bitcode);
        llvm::WriteBitcodeToFile(*pm, os);
        os.flush();
        llvm::BLAKE3 h;
        h.update(id + " level " + std::to_string(j.level) + "\n");
        h.update(llvm::StringRef(j.bitcode));
        auto digest = h.final();
        std::string key = hex(digest.data(), digest.size());
        if (!dir.empty()) {
            j.object = dir + "/" + key + ".o";
            j.cached = access(j.object.c_str(), R_OK) == 0;
            if (j.cached) utime(j.object.c_str(), nullptr);  // (used: kept another month)
        } else {
            j.object = tmp_base + "." + std::to_string(jobs.size()) + ".o";
            temps.push_back(j.object);
        }
        jobs.push_back(std::move(j));
    }

    // the parts not in the cache, compiled on as many threads as there are cores
    std::vector<Job *> todo;
    for (Job &j : jobs) {
        objects.push_back(j.object);
        if (!j.cached) todo.push_back(&j);
    }
    stats.parts = jobs.size();
    stats.compiled = todo.size();
    std::atomic<size_t> next{0};
    std::atomic<bool> ok{true};
    auto work = [&] {
        for (size_t i; (i = next++) < todo.size();) {
            Job &j = *todo[i];
            llvm::LLVMContext ctx;
            auto mod = llvm::parseBitcodeFile(llvm::MemoryBufferRef(j.bitcode, j.name), ctx);
            if (!mod) {
                llvm::consumeError(mod.takeError());
                fprintf(stderr, "lisc: internal error: part %s doesn't read back\n", j.name.c_str());
                ok = false;
                continue;
            }
            std::unique_ptr<llvm::TargetMachine> tm(t.machine(j.level));
            optimize(**mod, tm.get(), j.level);
            lower_aggregate_copies(**mod);
            // (written beside, then renamed: another lisc may be reading the cache)
            std::string tmp = j.object + ".tmp" + std::to_string(getpid()) + "." + std::to_string(i);
            if (!emit_object(**mod, tm.get(), tmp) || rename(tmp.c_str(), j.object.c_str()) != 0) {
                unlink(tmp.c_str());
                ok = false;
            }
        }
    };
    unsigned n = std::max(1u, std::min<unsigned>(std::thread::hardware_concurrency(), (unsigned)todo.size()));
    std::vector<std::thread> threads;
    for (unsigned k = 1; k < n; k++) threads.emplace_back(work);
    work();
    for (auto &th : threads) th.join();
    if (!dir.empty()) prune(dir);
    return ok;
}
