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
    bool imported = false;          // from a C module
};

class Compiler {
public:
    Compiler();
    ~Compiler();
    // compile a whole program; stops with an error message on the first problem
    void compile(const std::vector<Form *> &forms);
    bool debug = false;   // -g: DWARF debug info
    bool checks = false;  // --checks: bounds, casts and division checked at run time
    llvm::Module &module() { return *mod_; }

    // where import and import-c look (after the importing file's directory;
    // then lib_dir for modules, the system directories for headers)
    std::vector<std::string> include_dirs;
    std::string lib_dir;  // the standard library: lib/ next to the lisc binary
    bool c_cache = true;  // keep C imports in ~/.cache/lisc (--no-cache: don't)
    // -D NAME[=value]: an untyped constant (a number, or else a string; 1 without a value)
    void define_const(const std::string &name, const std::string &value);

    // used by the C header importer (cimport.cpp)
    // (names are put under c_prefix: the C module being imported, e.g. "libc::")
    std::string c_prefix;
    void add_type(const std::string &name, Type *t);
    bool has_type(const std::string &name) const { return types_.count(c_prefix + name) != 0; }
    bool has_global(const std::string &name) const { return globals_.count(c_prefix + name) != 0; }
    void add_extern_fn(const std::string &name, Type *fn);
    void add_extern_var(const std::string &name, Type *t);
    void add_const_int(const std::string &name, Type *t, uint64_t v);
    void add_const_float(const std::string &name, Type *t, double v);
    void add_const_str(const std::string &name, const std::string &s);
    void put_c(const std::string &name, Global g);

private:
    struct Impl;
    std::unique_ptr<llvm::LLVMContext> ctx_;
    std::unique_ptr<llvm::Module> mod_;
    std::unique_ptr<Impl> impl_;
    std::map<std::string, Type *> types_;
    std::map<std::string, Global> globals_;
    friend struct Impl;
};

// the C declarations of these headers, put under c.c_prefix (a C module)
void c_include(Compiler &c, const std::vector<std::string> &headers, Loc at);
