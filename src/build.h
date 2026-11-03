// build.h — from the program's LLVM module to object files (build.cpp).
#pragma once

#include <string>
#include <vector>

namespace llvm {
class Module;
class Target;
class TargetMachine;
}  // namespace llvm

// the machine code is made for: the host
struct Target {
    const llvm::Target *target;
    std::string triple, cpu, features;
    llvm::TargetMachine *machine(int level) const;  // a new one, for -O<level>
};

void optimize(llvm::Module &m, llvm::TargetMachine *tm, int level);
void lower_aggregate_copies(llvm::Module &m);
bool emit_object(llvm::Module &m, llvm::TargetMachine *tm, const std::string &path);

struct BuildStats {
    size_t parts = 0, compiled = 0;  // (the rest came from the cache)
};

// m split into parts, compiled in parallel (or taken from the object cache);
// objects: what to link; temps: files to delete after (when not caching)
bool build_parts(llvm::Module &m, const Target &t, int level, bool use_cache, const std::string &tmp_base,
                 std::vector<std::string> &objects, std::vector<std::string> &temps, BuildStats &stats);
