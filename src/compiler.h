// compiler.h — typed core forms → LLVM IR.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "form.h"
#include "type.h"

namespace llvm {
class LLVMContext;
class Module;
class Value;
class Type;
}

struct Global {
    enum Kind { Func, Var, Const } kind;
    Type *type = nullptr;           // Func: the fn type; Var/Const: the value type
    llvm::Value *value = nullptr;   // Function, GlobalVariable, or Constant
    Form *form = nullptr;           // Const without a type: its expression, compiled at each use
    Form *def = nullptr;            // Func: the defn form (null for externs)
};

class Compiler {
public:
    Compiler();
    ~Compiler();
    // compile a whole program; stops with an error message on the first problem
    void compile(const std::vector<Form *> &forms);
    llvm::Module &module() { return *mod_; }

    // where (c-include ...) looks first (then the system directories)
    std::vector<std::string> include_dirs;

    // used by the C header importer (cimport.cpp)
    void add_type(const std::string &name, Type *t);
    bool has_type(const std::string &name) const { return types_.count(name) != 0; }
    bool has_global(const std::string &name) const { return globals_.count(name) != 0; }
    void add_extern_fn(const std::string &name, Type *fn);
    void add_extern_var(const std::string &name, Type *t);
    void add_const_int(const std::string &name, Type *t, uint64_t v);
    void add_const_float(const std::string &name, Type *t, double v);
    void add_const_str(const std::string &name, const std::string &s);

private:
    struct Impl;
    std::unique_ptr<llvm::LLVMContext> ctx_;
    std::unique_ptr<llvm::Module> mod_;
    std::unique_ptr<Impl> impl_;
    std::map<std::string, Type *> types_;
    std::map<std::string, Global> globals_;
    friend struct Impl;
};

// (c-include "stdio.h" ...): make the C declarations of these headers usable
void c_include(Compiler &c, const std::vector<std::string> &headers, Loc at);
