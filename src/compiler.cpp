// compiler.cpp — type checking and LLVM code generation, in one pass over the
// typed core forms.
//
// Top level:
//   (import "m" [:as a])  (import-c "h" ... [:as a])  (using m ...)   modules, see "modules" below
//   (struct name (field T) ...)      also (union name ...)
//   (const NAME expr)                a compile-time constant; untyped if expr is
//   (def name T [init])              a global variable
//   (defn name ((a T) ...) R [:noinline|:inline|:cold|:optnone] body...)
//   (extern name (T ... [...]) R)    a C function by hand
//
// Expressions (everything is an expression; statements have type void):
//   literals, names, (f args...), (do e...), (let ((x [T] [init]) ...) body...)
//   (set place e)  (if c a [b])  (when c body...)  (unless c body...)
//   (while c body...)  (for (i [T] start end [step]) body...)  (break)  (continue)  (return [e])
//   (+ - * / % & | ^ << >>)  (- x)  (~ x)  (= != < <= > >=)  (and ...) (or ...) (not x)
//   (+= place e) ... (>>= place e): the place's address computed once
//   (atomic-load p) (atomic-store p v) (atomic-swap/add/sub/and/or/xor p v) (atomic-cas p old new) (fence)
//   (. x field ...)  (aref a i)  (deref p)  (addr place)  (cast T e)  (truncate T e)  (bitcast T e)
//   (sizeof T)  (alignof T)  (make T [:field e ...] | [e ...])
//
// Everything starts as zero: let bindings without a value, globals, the
// fields and elements make does not set. `null` is the zero value of the type
// wanted (the null pointer, 0, 0.0, false, an all-zero struct); `undefined`
// opts out: (let ((buf (array u8 4096) undefined)) ...) leaves memory as it is.
//
// Literals are untyped until used: 1.5 becomes an f32 where an f32 is
// expected (rounded straight from the decimal text, like C's 1.5f), and an
// f64 otherwise. Mixing typed operands of different types is an error; use
// (cast T e).

#include "compiler.h"
#include "macro.h"

#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

using llvm::BasicBlock;

Type *t_str();

namespace {

struct Val {
    llvm::Value *v = nullptr;
    Type *t = nullptr;
    uint64_t align = 0;  // for addresses: known alignment (0 = the type's)
    // a bit field: v is the address of the bytes holding it, read as one integer
    llvm::IntegerType *bwin = nullptr;
    unsigned bshift = 0, bwidth = 0;
};

// How a value crosses a call, per the x86-64 System V ABI.
struct ArgAbi {
    enum Kind { Direct, Coerce, Memory, Ignore } kind = Direct;
    std::vector<llvm::Type *> parts;  // Coerce: one register-sized piece per eightbyte
};
struct FnAbi {
    ArgAbi ret;
    std::vector<ArgAbi> params;
    llvm::FunctionType *ty = nullptr;
    llvm::AttributeList attrs;
};

struct Loop {
    BasicBlock *cont, *brk;
    size_t depth;  // scopes.size() outside the loop's body: break/continue run the defers inside
};

const char *const ARITH[] = {"+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>"};
const char *const CMP[] = {"=", "!=", "<", "<=", ">", ">="};

bool in(const char *s, const char *const *list, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!strcmp(s, list[i])) return true;
    return false;
}
// A local variable's name in the scopes: the symbol's text, plus its
// hygiene mark if a macro template introduced it. Globals, types, fields and
// keywords are looked up by text alone, so a template's free names mean the
// global definitions, never a local of the code the macro is used in.
std::string key(const Form *sym) { return sym->mark ? sym->text + "\x01" + std::to_string(sym->mark) : sym->text; }

bool is_arith(const char *s) { return in(s, ARITH, sizeof ARITH / sizeof *ARITH); }
bool is_cmp(const char *s) { return in(s, CMP, sizeof CMP / sizeof *CMP); }

}  // namespace

struct Compiler::Impl : MetaHost {
    Compiler &c;
    llvm::LLVMContext &ctx;
    llvm::Module &mod;
    llvm::IRBuilder<> b;
    llvm::Function *fn = nullptr;
    Type *fn_ret = nullptr;
    Type *cur_ft = nullptr;            // the current function's type
    llvm::Value *cur_sret = nullptr;   // where a struct result returned in memory goes
    // a block's locals (name → address) and the modules it is using
    struct Scope {
        std::map<std::string, Val> vars;
        std::vector<std::string> usings;
        std::vector<Form *> defers;  // (defer ...) in this block, in order
        std::vector<Form *> declared;  // let/for names bound here, for the unused-variable warning
        std::set<std::string> used;
        Val &operator[](const std::string &k) { return vars[k]; }
    };
    std::vector<Scope> scopes;
    std::vector<Loop> loops;
    std::map<StructInfo *, bool> laying_out;          // cycle guard
    Meta meta{*this};
    // defop: operator → its definitions (functions named like "+(v3,v3)")
    std::map<std::string, std::vector<Global *>> overloads;

    Impl(Compiler &c) : c(c), ctx(*c.ctx_), mod(*c.mod_), b(ctx) {}

    /* ---------------------------------------------------- debug info --- */
    // lisc -g: DWARF for gdb. The program is described as C (so gdb's print
    // uses C expressions); code a macro produced is at the macro call's line.

    std::unique_ptr<llvm::DIBuilder> dib;
    llvm::DICompileUnit *cu = nullptr;
    llvm::DISubprogram *dsp = nullptr;  // the function being compiled
    std::map<std::string, llvm::DIFile *> difiles;
    std::map<Type *, llvm::DIType *> ditypes;

    static bool real_file(const char *f) { return f && f[0] && f[0] != '<' && f[0] != '?'; }

    void debug_init(const char *main_file) {
        dib.reset(new llvm::DIBuilder(mod));
        cu = dib->createCompileUnit(llvm::dwarf::DW_LANG_C99, difile(main_file), "lisc", false, "", 0);
        mod.addModuleFlag(llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION);
        mod.addModuleFlag(llvm::Module::Warning, "Dwarf Version", 4);
    }

    llvm::DIFile *difile(const char *path) {
        llvm::DIFile *&f = difiles[path];
        if (!f) {
            char real[4096];
            std::string p = realpath(path, real) ? real : path;
            size_t s = p.rfind('/');
            f = dib->createFile(s == std::string::npos ? p : p.substr(s + 1), s == std::string::npos ? "." : p.substr(0, s));
        }
        return f;
    }

    llvm::DIType *ditype(Type *t) {
        auto it = ditypes.find(t);
        if (it != ditypes.end()) return it->second;
        llvm::DIType *d = nullptr;
        llvm::DIFile *f0 = cu->getFile();
        switch (t->kind) {
        case Type::Void: return nullptr;
        case Type::Bool: d = dib->createBasicType("bool", 8, llvm::dwarf::DW_ATE_boolean); break;
        case Type::Int:
            if (t->en) {
                std::vector<llvm::Metadata *> es;
                for (auto &v : t->en->values) es.push_back(dib->createEnumerator(t->en->name + "." + v.first, v.second, !t->sign));
                Type *base = t_int(t->bits, t->sign);
                d = dib->createEnumerationType(cu, t->en->name, f0, 0, t->bits, t->bits, dib->getOrCreateArray(es), ditype(base));
            } else {
                d = dib->createBasicType(type_name(t), t->bits, t->sign ? llvm::dwarf::DW_ATE_signed : llvm::dwarf::DW_ATE_unsigned);
            }
            break;
        case Type::Float: d = dib->createBasicType(type_name(t), t->bits, llvm::dwarf::DW_ATE_float); break;
        case Type::Ptr: d = dib->createPointerType(t->elem == t_void() ? nullptr : ditype(t->elem), 64); break;
        case Type::Array: {
            llvm::Metadata *sub[] = {dib->getOrCreateSubrange(0, (int64_t)t->n)};
            d = dib->createArrayType(size_of(t) * 8, align_of(t) * 8, ditype(t->elem), dib->getOrCreateArray(sub));
            break;
        }
        case Type::Fn: {
            std::vector<llvm::Metadata *> ps{ditype(t->elem)};
            for (Type *p : t->params) ps.push_back(ditype(p));
            d = dib->createSubroutineType(dib->getOrCreateTypeArray(ps));
            break;
        }
        case Type::Struct: {
            StructInfo *st = t->st;
            auto tag = st->is_union ? llvm::dwarf::DW_TAG_union_type : llvm::dwarf::DW_TAG_structure_type;
            if (!st->complete) {  // opaque: only behind pointers
                d = dib->createForwardDecl(tag, st->name, cu, f0, 0);
                break;
            }
            // (through a placeholder first: a struct may point to itself)
            llvm::DICompositeType *fwd = dib->createReplaceableCompositeType(tag, st->name, cu, f0, 0, 0, st->size * 8, st->align * 8);
            ditypes[t] = fwd;
            std::vector<llvm::Metadata *> ms;
            for (auto &fd : st->fields) {
                if (fd.bit_width)
                    ms.push_back(dib->createBitFieldMemberType(fwd, fd.name, f0, 0, fd.bit_width, fd.bit_off, fd.offset * 8,
                                                               llvm::DINode::FlagZero, ditype(fd.type)));
                else
                    ms.push_back(dib->createMemberType(fwd, fd.name, f0, 0, size_of(fd.type) * 8, align_of(fd.type) * 8, fd.offset * 8,
                                                       llvm::DINode::FlagZero, ditype(fd.type)));
            }
            llvm::DICompositeType *real =
                st->is_union ? dib->createUnionType(cu, st->name, f0, 0, st->size * 8, st->align * 8, llvm::DINode::FlagZero, dib->getOrCreateArray(ms))
                             : dib->createStructType(cu, st->name, f0, 0, st->size * 8, st->align * 8, llvm::DINode::FlagZero, nullptr,
                                                     dib->getOrCreateArray(ms));
            dib->replaceTemporary(llvm::TempDIType(fwd), real);
            d = real;
            break;
        }
        }
        return ditypes[t] = d;
    }

    /* ---------------------------------------------------- run-time checks --- */
    // lisc --checks: unless `ok`, print "file:line:col: what" (printf-style, with
    // `args`) to stderr and abort
    void check(llvm::Value *ok, Loc at, const std::string &what, std::vector<llvm::Value *> args = {}) {
        if (!c.checks || !fn) return;
        BasicBlock *fail = BasicBlock::Create(ctx, "check.fail", fn), *cont = BasicBlock::Create(ctx, "check.ok", fn);
        b.CreateCondBr(ok, cont, fail);
        b.SetInsertPoint(fail);
        Loc u = user_loc(at);
        std::string file;
        for (const char *p = u.file; *p; p++) file += *p == '%' ? std::string("%%") : std::string(1, *p);
        std::string msg = file + ":" + std::to_string(u.line) + ":" + std::to_string(u.col) + ": " + what + "\n";
        auto *dp = llvm::FunctionType::get(b.getInt32Ty(), {b.getInt32Ty(), b.getPtrTy()}, true);
        std::vector<llvm::Value *> as = {b.getInt32(2), b.CreateGlobalStringPtr(msg, "check", 0, &mod)};
        as.insert(as.end(), args.begin(), args.end());
        b.CreateCall(mod.getOrInsertFunction("dprintf", dp), as);
        b.CreateCall(mod.getOrInsertFunction("abort", llvm::FunctionType::get(b.getVoidTy(), false)));
        b.CreateUnreachable();
        b.SetInsertPoint(cont);
    }

    // where generated code is: the outermost macro call, for code macros made
    static Loc user_loc(Loc l) {
        while (l.via) l = l.via->at;
        return l;
    }
    // the scope for a line in `file`: the function, or the function seen from another file
    llvm::DIScope *dscope_in(const char *file) {
        if (dsp->getFile() == difile(file)) return dsp;
        return dib->createLexicalBlockFile(dsp, difile(file));
    }
    void set_loc(Loc at) {
        if (!dsp) return;
        at = user_loc(at);
        if (!real_file(at.file) || at.line <= 0) return;
        b.SetCurrentDebugLocation(llvm::DILocation::get(ctx, at.line, at.col, dscope_in(at.file)));
    }
    void declare_var(llvm::Value *addr, const std::string &name, Type *t, Loc at, unsigned argno = 0) {
        if (!dsp || !ditype(t)) return;
        at = user_loc(at);
        if (!real_file(at.file)) at = {dsp->getFile()->getFilename().data(), (int)dsp->getLine(), 0};
        llvm::DIScope *sc = real_file(at.file) ? dscope_in(at.file) : dsp;
        llvm::DIFile *file = real_file(at.file) ? difile(at.file) : dsp->getFile();
        llvm::DILocalVariable *v = argno ? dib->createParameterVariable(sc, name, argno, file, at.line, ditype(t), true)
                                         : dib->createAutoVariable(sc, name, file, at.line, ditype(t), true);
        dib->insertDeclare(addr, v, dib->createExpression(), llvm::DILocation::get(ctx, at.line, at.col, sc), b.GetInsertBlock());
    }

    /* ------------------------------------------------------- modules --- */
    // A module is a file. Its top-level names are stored as "module::name" in
    // the tables (globals, types, macros); C imports and -D constants keep
    // their plain names. A symbol is resolved in the module it was read from
    // (a macro template's symbols in the macro's module):
    //   name      the module's own name, else from a module it is using (in this
    //             block, then in the file), else a plain global (-D constants)
    //   m::name   from the module imported as m (one level: no m::n::x); not if private

    struct ModInfo {
        std::string name, path;
        std::map<std::string, std::string> alias;  // import alias → module name
        std::vector<std::string> usings;            // (using m) at the top level: aliases
        std::vector<std::string> headers;           // a C module: its headers (read in pass 0)
    };
    std::vector<std::string> c_mods;                // C modules, in the order they were imported

    // the C modules built in: (import "libc"), (import "posix") ...
    static const std::vector<std::string> *builtin_c(const std::string &name) {
        static const std::map<std::string, std::vector<std::string>> mods = {
            {"libc", {"stdio.h", "stdlib.h", "string.h", "ctype.h", "errno.h", "limits.h", "stdint.h", "stdbool.h"}},
            {"math", {"math.h"}},
            {"posix", {"unistd.h", "fcntl.h", "sys/stat.h", "sys/types.h"}},
            {"io", {"poll.h", "sys/ioctl.h", "sys/mman.h"}},
            {"signal", {"signal.h"}},
            {"time", {"time.h"}},
            {"thread", {"pthread.h"}},
        };
        auto it = mods.find(name);
        return it == mods.end() ? nullptr : &it->second;
    }

    ModInfo *c_module(const std::string &name, const std::vector<std::string> &headers, Loc at) {
        auto it = mods.find(name);
        if (it != mods.end()) {
            if (it->second.path != "<c>") fatal(at, "'%s' is already the name of module %s: give the C module another (:as name)", name.c_str(), it->second.path.c_str());
            if (it->second.headers != headers) fatal(at, "C module %s is already imported with other headers", name.c_str());
            return &it->second;
        }
        ModInfo &m = mods[name];
        m.name = name;
        m.path = "<c>";
        m.headers = headers;
        c_mods.push_back(name);
        return &m;
    }

    // the module an alias names, as seen from module m
    ModInfo *aliased(ModInfo *m, const std::string &a) {
        auto it = m->alias.find(a);
        if (it != m->alias.end()) return &mods[it->second];
        return a == m->name ? m : nullptr;
    }
    std::map<std::string, ModInfo> mods;            // by name
    std::set<std::string> priv;                     // qualified names defined private
    std::set<Form *> priv_names;                    // name symbols of private definitions
    std::vector<std::pair<Form *, ModInfo *>> items;  // every top-level form, with its module
    std::set<std::string> loaded_files;             // (include ...) reads a file once per module
    ModInfo *cur = nullptr;                         // module of the top-level form being compiled

    ModInfo *mod_of(Form *sym) { return sym->mod ? &mods[sym->mod] : cur; }

    // the qualified name a definition in the current module gets
    std::string def_key(Form *name) {
        if (name->text.find("::") != std::string::npos) fatal(name->loc, "a definition's name has no module part: %s", name->text.c_str());
        return cur ? cur->name + "::" + name->text : name->text;
    }

    // the table key a symbol refers to, or "" if `has` knows none
    // (same(a, b): do two keys name one thing? C headers include each other, so
    // two C modules can both declare size_t or NULL)
    std::string resolve(Form *sym, const std::function<bool(const std::string &)> &has,
                        const std::function<bool(const std::string &, const std::string &)> &same = nullptr) {
        const std::string &t = sym->text;
        ModInfo *m = mod_of(sym);
        size_t p = t.find("::");
        if (p != std::string::npos) {
            std::string a = t.substr(0, p), n = t.substr(p + 2);
            if (a.empty() || n.empty() || n.find("::") != std::string::npos) fatal(sym->loc, "'%s': a name has at most one module part", t.c_str());
            ModInfo *target = m ? aliased(m, a) : nullptr;
            if (!target) fatal(sym->loc, "no module '%s' is imported here", a.c_str());
            (void)0;
            std::string k = target->name + "::" + n;
            if (!has(k)) return "";
            if (target != m && priv.count(k)) fatal(sym->loc, "'%s' is private to module %s", n.c_str(), target->name.c_str());
            return k;
        }
        if (m) {
            std::string k = m->name + "::" + t;
            if (has(k)) return k;
            // a name from the modules in use: the innermost block's first, then the file's
            auto look = [&](const std::vector<std::string> &usings) -> std::string {
                std::string found;
                for (auto &a : usings) {
                    ModInfo *x = aliased(m, a);
                    if (!x) fatal(sym->loc, "using %s: no module '%s' is imported here", a.c_str(), a.c_str());
                    std::string k2 = x->name + "::" + t;
                    if (!has(k2) || priv.count(k2)) continue;
                    if (!found.empty() && found != k2 && !(same && same(found, k2)))
                        fatal(sym->loc, "'%s' is in both %s and %s: write %s or %s", t.c_str(), found.substr(0, found.find("::")).c_str(),
                              x->name.c_str(), found.c_str(), k2.c_str());
                    found = k2;
                }
                return found;
            };
            if (m == cur)  // (a macro template's symbols are not affected by the blocks they land in)
                for (auto sc = scopes.rbegin(); sc != scopes.rend(); ++sc)
                    if (!sc->usings.empty()) {
                        std::string f = look(sc->usings);
                        if (!f.empty()) return f;
                    }
            std::string f = look(m->usings);
            if (!f.empty()) return f;
        }
        return has(t) ? t : "";
    }
    std::string global_key(Form *sym) {
        return resolve(sym, [&](const std::string &k) { return c.globals_.count(k) != 0; },
                       [&](const std::string &a, const std::string &b) {
                           Global &x = c.globals_[a], &y = c.globals_[b];
                           if (!x.imported || !y.imported || x.kind != y.kind) return false;
                           if (x.value || y.value) {
                               if (x.value == y.value) return true;
                               auto *gx = llvm::dyn_cast_or_null<llvm::GlobalVariable>(x.value);
                               auto *gy = llvm::dyn_cast_or_null<llvm::GlobalVariable>(y.value);
                               // C string constants: one name if the strings are the same
                               return x.kind == Global::Const && gx && gy && gx->hasInitializer() && gy->hasInitializer() &&
                                      gx->getInitializer() == gy->getInitializer();
                           }
                           return x.form && y.form && x.form->kind == y.form->kind && x.form->text == y.form->text;
                       });
    }
    std::string type_key(Form *sym) {
        return resolve(sym, [&](const std::string &k) { return c.types_.count(k) != 0; },
                       [&](const std::string &a, const std::string &b) { return c.types_[a] == c.types_[b]; });
    }
    std::string macro_key(Form *sym) { return resolve(sym, [&](const std::string &k) { return meta.is_macro(k); }); }

    // every symbol of a file's forms belongs to its module
    void tag(Form *f, const char *m) {
        if (f->kind == Form::Sym && !f->mod) f->mod = m;
        for (Form *x : f->items) tag(x, m);
    }

    std::string near(Form *at, const std::string &path) {
        std::string from = at->loc.file;
        if (path[0] == '/' || from.find('/') == std::string::npos) return path;
        return from.substr(0, from.rfind('/') + 1) + path;
    }

    // where (import "name") is: next to the importing file, else in the -I
    // directories, else in the library next to lisc (lib/); .lsp is optional
    std::string find_module(Form *at, std::string name) {
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".lsp") != 0) name += ".lsp";
        std::string here = near(at, name);
        if (name[0] == '/' || access(here.c_str(), F_OK) == 0) return here;
        for (auto &d : c.include_dirs) {
            std::string p = d + "/" + name;
            if (access(p.c_str(), F_OK) == 0) return p;
        }
        if (!c.lib_dir.empty() && access((c.lib_dir + "/" + name).c_str(), F_OK) == 0) return c.lib_dir + "/" + name;
        return here;  // (the error says where it looked first)
    }

    // a module's file, read once; its forms (and its imports') go to items
    ModInfo *load_module(const std::string &path, Loc at, std::vector<Form *> *forms = nullptr) {
        char real[4096];
        if (!realpath(path.c_str(), real)) fatal(at, "cannot import %s: %s", path.c_str(), strerror(errno));
        std::string name = path.substr(path.find('/') == std::string::npos ? 0 : path.rfind('/') + 1);
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".lsp") == 0) name.resize(name.size() - 4);
        auto it = mods.find(name);
        if (it != mods.end()) {
            if (it->second.path != real) fatal(at, "two modules named %s: %s and %s", name.c_str(), it->second.path.c_str(), real);
            return &it->second;
        }
        ModInfo &m = mods[name];
        m.name = name;
        m.path = real;
        std::vector<Form *> fs = forms ? *forms : read_file(strdup(path.c_str()));
        for (Form *f : fs) tag(f, m.name.c_str());
        add_forms(fs, &m);
        return &m;
    }

    void add_forms(const std::vector<Form *> &fs, ModInfo *m) {
        for (Form *f : fs) {
            if (f->is_call("import")) {
                // (import "path" [:as alias]); the path is relative to this file, .lsp optional;
                // "libc" and "math" are the C modules built in
                if (f->size() < 2 || f->at(1)->kind != Form::Str) fatal(f->loc, "(import \"module\" [:as name])");
                std::string as;
                for (size_t i = 2; i < f->size(); i++) {
                    if (f->at(i)->is_sym(":as") && i + 1 < f->size() && f->at(i + 1)->kind == Form::Sym) as = f->at(++i)->text;
                    else if (f->at(i)->is_sym(":all")) {
                        std::string n = f->at(1)->text;
                        n = n.substr(n.find('/') == std::string::npos ? 0 : n.rfind('/') + 1);
                        fatal(f->at(i)->loc, ":all is gone: import it, then (using %s)", n.c_str());
                    }
                    else fatal(f->at(i)->loc, "(import \"module\" [:as name])");
                }
                ModInfo *t;
                if (const std::vector<std::string> *hs = builtin_c(f->at(1)->text)) t = c_module(f->at(1)->text, *hs, f->loc);
                else {
                    t = load_module(find_module(f, f->at(1)->text), f->loc);
                }
                m->alias[as.empty() ? t->name : as] = t->name;
            } else if (f->is_call("import-c")) {
                // (import-c "header.h" ... [:as name]): C declarations as a module, named
                // after the first header unless given
                std::vector<std::string> hs;
                std::string as;
                for (size_t i = 1; i < f->size(); i++) {
                    if (f->at(i)->kind == Form::Str) hs.push_back(f->at(i)->text);
                    else if (f->at(i)->is_sym(":as") && i + 1 < f->size() && f->at(i + 1)->kind == Form::Sym) as = f->at(++i)->text;
                    else fatal(f->at(i)->loc, "(import-c \"header.h\" ... [:as name])");
                }
                if (hs.empty()) fatal(f->loc, "(import-c \"header.h\" ... [:as name])");
                if (as.empty()) {
                    as = hs[0].substr(hs[0].find('/') == std::string::npos ? 0 : hs[0].rfind('/') + 1);
                    if (as.size() > 2 && as.compare(as.size() - 2, 2, ".h") == 0) as.resize(as.size() - 2);
                }
                m->alias[as] = c_module(as, hs, f->loc)->name;
            } else if (f->is_call("using")) {
                // (using m ...) at the top level: m's names unqualified in the whole file
                for (size_t i = 1; i < f->size(); i++) {
                    if (f->at(i)->kind != Form::Sym) fatal(f->at(i)->loc, "(using module ...)");
                    m->usings.push_back(f->at(i)->text);
                }
            } else if (f->is_call("c-include")) {
                fatal(f->loc, "c-include is gone: (import \"libc\"), (import \"math\") or (import-c \"header.h\" :as name)");
            } else if (f->is_call("include")) {
                // (include "file.lsp" ...): that file's forms as part of this module
                for (size_t i = 1; i < f->size(); i++) {
                    if (f->at(i)->kind != Form::Str) fatal(f->at(i)->loc, "(include \"file.lsp\" ...)");
                    std::string path = near(f, f->at(i)->text);
                    char real[4096];
                    if (!realpath(path.c_str(), real)) fatal(f->at(i)->loc, "cannot include %s: %s", path.c_str(), strerror(errno));
                    if (!loaded_files.insert(m->name + ":" + real).second) continue;
                    std::vector<Form *> inc = read_file(strdup(path.c_str()));
                    for (Form *x : inc) tag(x, m->name.c_str());
                    add_forms(inc, m);
                }
            } else if (f->is_call("private")) {
                // (private form ...): definitions only this module can use
                std::vector<Form *> inner(f->items.begin() + 1, f->items.end());
                for (Form *x : inner) mark_private(x);
                add_forms(inner, m);
            } else {
                static const char *const dash[] = {"defn-", "def-", "const-", "defmacro-", "struct-", "union-"};
                for (const char *d : dash)
                    if (f->is_call(d)) {  // (defn- name ...) = (private (defn name ...))
                        std::vector<Form *> it = f->items;
                        std::string h = d;
                        it[0] = mk_sym(f->at(0)->loc, h.substr(0, h.size() - 1));
                        f = mk_list(f->loc, it);
                        mark_private(f);
                    }
                items.push_back({f, m});
            }
        }
    }

    void mark_private(Form *f) {
        if (f->is_list() && f->size() >= 2 && f->at(1)->kind == Form::Sym) priv_names.insert(f->at(1));
        else if (f->is_list() && f->size() >= 2 && f->at(1)->is_list() && f->at(1)->size() && f->at(1)->at(0)->kind == Form::Sym)
            priv_names.insert(f->at(1)->at(0));  // a generic's name
    }
    void note_private(Form *name, const std::string &key) {
        if (priv_names.count(name)) priv.insert(key);
    }

    /* -------------------------------------------------------- macros --- */

    // f with its macro calls at the head expanded (repeatedly)
    Form *expand(Form *f) {
        while (f->kind == Form::List && !f->items.empty() && f->at(0)->kind == Form::Sym) {
            std::string k = macro_key(f->at(0));
            if (k.empty()) break;
            f = meta.expand(f, k);
        }
        return f;
    }

    // the type of an expression where it stands, without emitting code: it is
    // compiled into a scratch block that is thrown away
    std::map<Form *, Type *> type_memo;  // type-of results in the function being compiled
    Type *type_of(Form *f) override {
        if (!fn) fatal(f->loc, "type-of works only inside functions");
        auto memo = type_memo.find(f);
        if (memo != type_memo.end()) return memo->second;
        llvm::IRBuilderBase::InsertPoint ip = b.saveIP();
        size_t nblocks = fn->size();
        b.SetInsertPoint(BasicBlock::Create(ctx, "typeof", fn));
        Type *t = expr(f, nullptr).t;
        std::vector<BasicBlock *> made;
        size_t i = 0;
        for (BasicBlock &bb : *fn)
            if (i++ >= nblocks) made.push_back(&bb);
        for (BasicBlock *bb : made) bb->dropAllReferences();
        for (BasicBlock *bb : made) bb->eraseFromParent();
        b.restoreIP(ip);
        return type_memo[f] = t;
    }

    /* --------------------------------------------------------- types --- */

    llvm::Type *ll(Type *t) {
        switch (t->kind) {
        case Type::Void: return b.getVoidTy();
        case Type::Bool: return b.getInt1Ty();
        case Type::Int: return b.getIntNTy(t->bits);
        case Type::Float: return t->bits == 32 ? b.getFloatTy() : b.getDoubleTy();
        case Type::Ptr: return b.getPtrTy();
        case Type::Array: return llvm::ArrayType::get(ll(t->elem), t->n);
        case Type::Struct: return ll_struct(t->st);
        case Type::Fn: return fn_abi(t).ty;
        }
        return nullptr;
    }

    /* ------------------------------------------- calling convention --- */
    // Every function, lisc's own included, uses the C calling convention, so
    // C can call lisc functions through pointers. Scalars pass directly
    // (small integers extended, as C expects); a struct of up to 16 bytes
    // passes as one or two register-sized pieces, each INTEGER or SSE by what
    // it holds; bigger structs (or ones that no longer fit in the remaining
    // registers) go through memory: a copy on the stack (byval) for arguments,
    // a pointer to the caller's space (sret) for results.

    std::map<Type *, FnAbi> abis;

    // the scalars inside t, with their byte offsets
    void leaves(Type *t, uint64_t off, std::vector<std::pair<uint64_t, Type *>> &out) {
        if (t->kind == Type::Struct) {
            for (auto &f : t->st->fields) {
                if (f.bit_width) {
                    for (uint64_t b0 = f.bit_off / 8; b0 <= (f.bit_off + f.bit_width - 1) / 8; b0++) out.push_back({off + b0, t_int(8, false)});
                } else leaves(f.type, off + f.offset, out);
            }
        } else if (t->kind == Type::Array) {
            for (uint64_t i = 0; i < t->n; i++) leaves(t->elem, off + i * size_of(t->elem), out);
        } else out.push_back({off, t});
    }

    ArgAbi classify(Type *t) {
        ArgAbi a;
        if (t->kind != Type::Struct) return a;  // Direct
        uint64_t size = size_of(t);
        if (size == 0) { a.kind = ArgAbi::Ignore; return a; }
        a.kind = ArgAbi::Memory;
        if (size > 16) return a;
        enum { NONE, INT, SSE } cls[2] = {NONE, NONE};
        std::vector<std::pair<uint64_t, Type *>> ls;
        leaves(t, 0, ls);
        for (auto &l : ls) {
            uint64_t eb = l.first / 8;
            if ((l.first + size_of(l.second) - 1) / 8 != eb) return a;  // straddles two eightbytes
            auto c = l.second->is_float() ? SSE : INT;
            if (cls[eb] == NONE || (cls[eb] == SSE && c == INT)) cls[eb] = c;
        }
        a.kind = ArgAbi::Coerce;
        for (uint64_t eb = 0; eb * 8 < size; eb++) {
            uint64_t bytes = std::min<uint64_t>(8, size - eb * 8);
            if (cls[eb] != SSE) { a.parts.push_back(b.getIntNTy(8 * bytes)); continue; }
            bool dbl = false, f0 = false, f4 = false;
            for (auto &l : ls) {
                if (l.first / 8 != eb) continue;
                if (l.second->bits == 64) dbl = true;
                else if (l.first % 8 == 0) f0 = true;
                else f4 = true;
            }
            if (dbl) a.parts.push_back(b.getDoubleTy());
            else if (f0 && !f4) a.parts.push_back(b.getFloatTy());
            else a.parts.push_back(llvm::FixedVectorType::get(b.getFloatTy(), 2));
        }
        return a;
    }

    FnAbi &fn_abi(Type *ft) {
        auto it = abis.find(ft);
        if (it != abis.end()) return it->second;
        FnAbi a;
        std::vector<llvm::Type *> irps;
        std::vector<llvm::AttributeSet> sets;  // per IR parameter
        llvm::AttrBuilder rattrs(ctx);
        int gpr = 6, sse = 8;  // argument registers left
        auto ext = [&](llvm::AttrBuilder &ab, Type *t) {
            if (t->kind == Type::Bool || (t->is_int() && t->bits < 32)) ab.addAttribute(t->kind == Type::Int && t->sign ? llvm::Attribute::SExt : llvm::Attribute::ZExt);
        };
        Type *rt = ft->elem;
        llvm::Type *irret = rt == t_void() ? b.getVoidTy() : nullptr;
        if (rt != t_void()) {
            a.ret = classify(rt);
            switch (a.ret.kind) {
            case ArgAbi::Direct: irret = ll(rt); ext(rattrs, rt); break;
            case ArgAbi::Ignore: irret = b.getVoidTy(); break;
            case ArgAbi::Coerce:
                irret = a.ret.parts.size() == 1 ? a.ret.parts[0] : llvm::StructType::get(ctx, a.ret.parts);
                break;
            case ArgAbi::Memory: {
                irret = b.getVoidTy();
                irps.push_back(b.getPtrTy());
                llvm::AttrBuilder ab(ctx);
                ab.addStructRetAttr(ll(rt));
                ab.addAlignmentAttr(align_of(rt));
                ab.addAttribute(llvm::Attribute::NoAlias);
                sets.push_back(llvm::AttributeSet::get(ctx, ab));
                gpr--;
                break;
            }
            }
        }
        for (Type *p : ft->params) {
            ArgAbi pa = classify(p);
            if (pa.kind == ArgAbi::Coerce) {
                int need_int = 0, need_sse = 0;
                for (llvm::Type *part : pa.parts) (part->isIntegerTy() ? need_int : need_sse)++;
                if (need_int <= gpr && need_sse <= sse) { gpr -= need_int; sse -= need_sse; }
                else { pa.kind = ArgAbi::Memory; pa.parts.clear(); }
            }
            switch (pa.kind) {
            case ArgAbi::Direct: {
                irps.push_back(ll(p));
                llvm::AttrBuilder ab(ctx);
                ext(ab, p);
                sets.push_back(llvm::AttributeSet::get(ctx, ab));
                if (p->is_float()) sse--; else gpr--;
                break;
            }
            case ArgAbi::Coerce:
                for (llvm::Type *part : pa.parts) {
                    irps.push_back(part);
                    sets.emplace_back();
                }
                break;
            case ArgAbi::Memory: {
                irps.push_back(b.getPtrTy());
                llvm::AttrBuilder ab(ctx);
                ab.addByValAttr(ll(p));
                ab.addAlignmentAttr(std::max<uint64_t>(8, align_of(p)));
                sets.push_back(llvm::AttributeSet::get(ctx, ab));
                break;
            }
            case ArgAbi::Ignore: break;
            }
            a.params.push_back(pa);
        }
        a.ty = llvm::FunctionType::get(irret, irps, ft->variadic);
        a.attrs = llvm::AttributeList::get(ctx, llvm::AttributeSet(), llvm::AttributeSet::get(ctx, rattrs), sets);
        return abis[ft] = a;
    }

    // byte offset `off` inside the memory at p
    llvm::Value *at_offset(llvm::Value *p, uint64_t off) { return off ? b.CreateInBoundsGEP(b.getInt8Ty(), p, b.getInt64(off)) : p; }

    // a struct value as its register pieces
    void coerce_out(const ArgAbi &abi, Val v, std::vector<llvm::Value *> &out) {
        llvm::AllocaInst *m = temp(v.t);
        store({m, v.t}, v.v);
        for (size_t i = 0; i < abi.parts.size(); i++)
            out.push_back(b.CreateAlignedLoad(abi.parts[i], at_offset(m, 8 * i), llvm::Align(std::min<uint64_t>(8, align_of(v.t)))));
        done(v.t, m);
    }
    // register pieces stored into a struct's memory
    void coerce_in(const ArgAbi &abi, const std::vector<llvm::Value *> &parts, llvm::Value *mem, Type *t) {
        for (size_t i = 0; i < parts.size(); i++)
            b.CreateAlignedStore(parts[i], at_offset(mem, 8 * i), llvm::Align(std::min<uint64_t>(8, align_of(t))));
        (void)abi;
    }

    // return v from the current function, lowered per its ABI
    void emit_ret(Val v) {
        const ArgAbi &r = fn_abi(cur_ft).ret;
        switch (r.kind) {
        case ArgAbi::Direct: b.CreateRet(v.v); break;
        case ArgAbi::Ignore: b.CreateRetVoid(); break;
        case ArgAbi::Memory:
            store({cur_sret, v.t}, v.v);
            b.CreateRetVoid();
            break;
        case ArgAbi::Coerce: {
            std::vector<llvm::Value *> parts;
            coerce_out(r, v, parts);
            if (parts.size() == 1) { b.CreateRet(parts[0]); break; }
            llvm::Value *agg = llvm::UndefValue::get(fn->getReturnType());
            for (unsigned i = 0; i < parts.size(); i++) agg = b.CreateInsertValue(agg, parts[i], i);
            b.CreateRet(agg);
            break;
        }
        }
    }

    // a packed LLVM struct with explicit padding, so the layout is exactly
    // the C one (fields are reached by byte offset, see field_addr)
    llvm::Type *ll_struct(StructInfo *st) {
        if (st->ll) return st->ll;
        if (!st->complete) return b.getInt8Ty();  // opaque: only usable behind a pointer
        std::vector<llvm::Type *> parts;
        uint64_t cur = 0;
        std::vector<const Field *> order;
        for (auto &f : st->fields)
            if (!f.bit_width) order.push_back(&f);  // bit fields live in the padding bytes
        std::stable_sort(order.begin(), order.end(), [](const Field *a, const Field *x) { return a->offset < x->offset; });
        for (const Field *f : order) {
            if (f->offset < cur) continue;  // overlaps (union member): reached by offset only
            if (f->offset > cur) parts.push_back(llvm::ArrayType::get(b.getInt8Ty(), f->offset - cur));
            parts.push_back(ll(f->type));
            cur = f->offset + size_of(f->type);
        }
        if (st->size > cur) parts.push_back(llvm::ArrayType::get(b.getInt8Ty(), st->size - cur));
        st->ll = llvm::StructType::get(ctx, parts, true);
        return st->ll;
    }

    void ensure_layout(StructInfo *st, Loc at) {
        if (st->complete) return;
        fatal(at, "struct %s is used before its fields are known", st->name.c_str());
    }

    /* ------------------------------------------------------ generics --- */

    // (struct (name T ...) fields...) and (defn (name T ...) params R body...):
    // specialised for each list of types they are used with. Inside, T is
    // bound to the type itself (not its name), so the body sees the same type
    // whatever module it was named in.
    struct Generic {
        Form *def;
        ModInfo *mod;
        std::string key;                  // module::name
        std::vector<std::string> params;  // T ...
        std::map<std::vector<Type *>, Type *> structs;
    };
    std::map<std::string, Generic> generic_structs, generic_fns;
    std::map<StructInfo *, std::pair<Generic *, std::vector<Type *>>> instance_of;
    std::map<std::string, Type *> tbind;  // the type parameters where code is being compiled
    struct PendingFn {
        Form *def;
        std::string key, shown;
        ModInfo *mod;
        std::map<std::string, Type *> bind;
        Loc at;  // where it was first used
    };
    std::vector<PendingFn> pending_fns;  // instances declared, bodies to compile
    bool structs_ready = false;          // pass 1 is over: instances can be laid out when made
    std::vector<std::pair<StructInfo *, Loc>> pending_layout;

    void register_generic(Form *f, bool is_fn) {
        Form *n = f->at(1);
        if (n->size() < 2 || n->at(0)->kind != Form::Sym)
            fatal(n->loc, is_fn ? "(defn (name T ...) ((a T) ...) R body...)" : "(struct (name T ...) (field T) ...)");
        Generic g{f, cur, def_key(n->at(0)), {}, {}};
        for (size_t i = 1; i < n->size(); i++) {
            if (n->at(i)->kind != Form::Sym) fatal(n->at(i)->loc, "a type parameter is a name");
            for (auto &p : g.params)
                if (p == n->at(i)->text) fatal(n->at(i)->loc, "type parameter %s is there twice", p.c_str());
            g.params.push_back(n->at(i)->text);
        }
        auto &table = is_fn ? generic_fns : generic_structs;
        if (table.count(g.key) || (is_fn ? c.globals_.count(g.key) : c.types_.count(g.key)))
            fatal(n->loc, "'%s' is already defined", n->at(0)->text.c_str());
        table[g.key] = g;
        note_private(n->at(0), g.key);
    }
    std::string generic_key(Form *sym, std::map<std::string, Generic> &table) {
        if (sym->kind != Form::Sym) return "";
        return resolve(sym, [&](const std::string &k) { return table.count(k) != 0; });
    }
    static std::string bare(const std::string &key) { return key.substr(key.find("::") == std::string::npos ? 0 : key.rfind("::") + 2); }
    std::string instance_name(Generic &g, const std::vector<Type *> &args) {
        std::string s = "(" + bare(g.key);
        for (Type *t : args) s += " " + type_name(t);
        return s + ")";
    }
    std::map<std::string, Type *> binding(Generic &g, const std::vector<Type *> &args) {
        std::map<std::string, Type *> b;
        for (size_t i = 0; i < args.size(); i++) b[g.params[i]] = args[i];
        return b;
    }
    // compile something of a generic's: in its module, with its parameters bound,
    // apart from the function being compiled where it is used
    template <class F> void inside(Generic &g, const std::map<std::string, Type *> &bind, F fn) {
        ModInfo *saved_cur = cur;
        std::map<std::string, Type *> saved_bind = bind;
        std::swap(saved_bind, tbind);
        std::vector<Scope> saved_scopes;
        std::swap(saved_scopes, scopes);
        cur = g.mod;
        fn();
        cur = saved_cur;
        std::swap(saved_bind, tbind);
        std::swap(saved_scopes, scopes);
    }
    std::vector<Type *> type_args(Generic &g, Form *f) {  // (name T ...) written out
        if (f->size() - 1 != g.params.size())
            fatal(f->loc, "%s takes %zu type%s, got %zu", bare(g.key).c_str(), g.params.size(), g.params.size() == 1 ? "" : "s", f->size() - 1);
        std::vector<Type *> args;
        for (size_t i = 1; i < f->size(); i++) args.push_back(parse_type(f->at(i)));
        return args;
    }

    Type *instantiate_struct(Generic &g, const std::vector<Type *> &args, Loc at) {
        auto it = g.structs.find(args);
        if (it != g.structs.end()) return it->second;
        auto *st = new StructInfo;
        st->name = instance_name(g, args);
        st->is_union = g.def->is_call("union");
        Type *t = t_struct(st);
        g.structs[args] = t;  // (before its fields: they may point to it)
        instance_of[st] = {&g, args};
        inside(g, binding(g, args), [&] {
            std::vector<std::pair<Form *, StructInfo *>> one{{g.def, st}};
            std::vector<ModInfo *> ms{g.mod};
            add_fields(one, ms);
        });
        if (structs_ready) lay_out(st, at);
        else pending_layout.push_back({st, at});
        return t;
    }

    Global *instantiate_fn(Generic &g, const std::vector<Type *> &args, Loc at) {
        std::string key = g.key + "(";
        for (size_t i = 0; i < args.size(); i++) key += (i ? " " : "") + type_name(args[i]);
        key += ")";
        auto it = c.globals_.find(key);
        if (it != c.globals_.end()) return &it->second;
        std::string shown = instance_name(g, args);
        auto bind = binding(g, args);
        inside(g, bind, [&] { declare_fn(g.def, key); });
        pending_fns.push_back({g.def, key, shown, g.mod, bind, at});
        return &c.globals_[key];
    }

    // the type parameters a call's arguments imply: each parameter's type is
    // matched against its argument's (literals and null don't count; they
    // then take the type that was found)
    void match(Generic &g, Form *pat, Type *t, std::map<std::string, Type *> &b, Form *arg) {
        if (pat->kind == Form::Sym) {
            if (std::find(g.params.begin(), g.params.end(), pat->text) == g.params.end()) return;
            auto it = b.find(pat->text);
            if (it == b.end()) b[pat->text] = t;
            else if (it->second != t)
                fatal(arg->loc, "%s is %s here but %s in an earlier argument", pat->text.c_str(), type_name(t).c_str(), type_name(it->second).c_str());
            return;
        }
        if (!pat->is_list() || pat->size() < 2 || pat->at(0)->kind != Form::Sym) return;
        if ((pat->is_call("ptr") && t->is_ptr()) || (pat->is_call("array") && t->kind == Type::Array)) return match(g, pat->at(1), t->elem, b, arg);
        if (t->kind != Type::Struct) return;
        if (pat->is_call("slice") && t->st->slice_of) return match(g, pat->at(1), t->st->slice_of, b, arg);
        std::string gk = generic_key(pat->at(0), generic_structs);
        auto inst = instance_of.find(t->st);
        if (gk.empty() || inst == instance_of.end() || inst->second.first != &generic_structs[gk]) return;
        for (size_t j = 1; j < pat->size() && j - 1 < inst->second.second.size(); j++) match(g, pat->at(j), inst->second.second[j - 1], b, arg);
    }
    Global *infer_fn(Generic &g, Form *call) {
        Form *ps = g.def->at(2);
        size_t n = call->size() - 1;
        if (n != ps->size()) fatal(call->loc, "%s takes %zu argument%s, got %zu", bare(g.key).c_str(), ps->size(), ps->size() == 1 ? "" : "s", n);
        std::map<std::string, Type *> b;
        for (size_t i = 0; i < n; i++) {
            Form *a = call->at(i + 1);
            if (untyped(a) || a->is_sym("null")) continue;
            if (ps->at(i)->is_list() && ps->at(i)->size() == 2) match(g, ps->at(i)->at(1), type_of(a), b, a);
        }
        // a parameter only literals stand for: their own type ((max 1 2) is for i32)
        for (size_t i = 0; i < n; i++) {
            Form *a = call->at(i + 1), *pt = ps->at(i)->is_list() && ps->at(i)->size() == 2 ? ps->at(i)->at(1) : nullptr;
            if (pt && pt->kind == Form::Sym && untyped(a) && !b.count(pt->text) &&
                std::find(g.params.begin(), g.params.end(), pt->text) != g.params.end())
                b[pt->text] = default_type(a);
        }
        std::vector<Type *> args;
        for (auto &p : g.params) {
            if (!b.count(p)) fatal(call->loc, "the arguments don't tell what %s is: write ((%s %s ...) ...)", p.c_str(), bare(g.key).c_str(), "T");
            args.push_back(b[p]);
        }
        return instantiate_fn(g, args, call->loc);
    }

    Type *parse_type(Form *f) override {
        f = expand(f);
        if (f->kind == Form::Sym) {
            auto tb = tbind.find(f->text);
            if (tb != tbind.end()) return tb->second;
            static const std::map<std::string, Type *> builtin = {
                {"void", t_void()}, {"bool", t_bool()},
                {"i8", t_int(8, true)}, {"i16", t_int(16, true)}, {"i32", t_int(32, true)}, {"i64", t_int(64, true)},
                {"u8", t_int(8, false)}, {"u16", t_int(16, false)}, {"u32", t_int(32, false)}, {"u64", t_int(64, false)},
                {"isize", t_int(64, true)}, {"usize", t_int(64, false)},
                {"f32", t_float(32)}, {"f64", t_float(64)}, {"str", t_str()},
            };
            auto it = builtin.find(f->text);
            if (it != builtin.end()) return it->second;
            std::string tk = type_key(f);
            if (!tk.empty()) return c.types_[tk];
            fatal(f->loc, "unknown type '%s'", f->text.c_str());
        }
        if (f->is_call("ptr") && f->size() == 2) return t_ptr(parse_type(f->at(1)));
        if (f->is_call("slice") && f->size() == 2) return t_slice(parse_type(f->at(1)));
        if (f->is_call("array") && f->size() == 3) {
            Val n = expr(f->at(2), t_int(64, false));
            auto *k = llvm::dyn_cast<llvm::ConstantInt>(n.v);
            if (!k || !n.t->is_int()) fatal(f->at(2)->loc, "array length must be an integer constant");
            return t_array(parse_type(f->at(1)), k->getZExtValue());
        }
        if (f->is_call("fn") && f->size() == 3 && f->at(1)->is_list()) {
            std::vector<Type *> ps;
            bool variadic = false;
            for (Form *p : f->at(1)->items) {
                if (p->is_sym("...")) variadic = true;
                else ps.push_back(parse_type(p));
            }
            return t_fn(parse_type(f->at(2)), ps, variadic);
        }
        if (f->is_list() && f->size() >= 2) {  // (name T ...): a generic struct for these types
            std::string gk = generic_key(f->at(0), generic_structs);
            if (!gk.empty()) {
                Generic &g = generic_structs[gk];
                return instantiate_struct(g, type_args(g, f), f->loc);
            }
        }
        fatal(f->loc, "bad type %s", show(f).c_str());
    }

    /* ------------------------------------------------------- helpers --- */

    [[noreturn]] void type_error(Loc at, const char *what, Type *want, Type *got) {
        fatal(at, "%s: expected %s, got %s", what, type_name(want).c_str(), type_name(got).c_str());
    }

    llvm::AllocaInst *alloca_(Type *t, const std::string &name) {
        llvm::IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
        llvm::AllocaInst *a = eb.CreateAlloca(ll(t), nullptr, name);
        a->setAlignment(llvm::Align(align_of(t)));
        return a;
    }

    llvm::Value *load(Val addr) {
        if (addr.bwidth) return load_bits(addr);
        return b.CreateAlignedLoad(ll(addr.t), addr.v, llvm::Align(addr.align ? addr.align : align_of(addr.t)));
    }
    void store(Val addr, llvm::Value *v) {
        if (addr.bwidth) return store_bits(addr, v);
        b.CreateAlignedStore(v, addr.v, llvm::Align(addr.align ? addr.align : align_of(addr.t)));
    }

    // bit fields: read the bytes holding the field as one integer (little
    // endian: the field's bits are bshift..bshift+bwidth-1), then shift/mask
    llvm::Value *load_bits(Val a) {
        unsigned n = a.bwin->getBitWidth();
        llvm::Value *w = b.CreateAlignedLoad(a.bwin, a.v, llvm::Align(1));
        if (a.t->is_int() && a.t->sign) {  // sign-extend from the field's top bit
            w = b.CreateShl(w, n - a.bshift - a.bwidth);
            w = b.CreateAShr(w, n - a.bwidth);
        } else {
            w = b.CreateLShr(w, a.bshift);
            w = b.CreateAnd(w, llvm::APInt::getLowBitsSet(n, a.bwidth));
        }
        if (a.t->kind == Type::Bool) return b.CreateICmpNE(w, llvm::ConstantInt::get(a.bwin, 0));
        return b.CreateIntCast(w, ll(a.t), a.t->sign);
    }
    void store_bits(Val a, llvm::Value *v) {
        unsigned n = a.bwin->getBitWidth();
        llvm::APInt mask = llvm::APInt::getBitsSet(n, a.bshift, a.bshift + a.bwidth);
        llvm::Value *nv = a.t->kind == Type::Bool ? b.CreateZExt(v, a.bwin) : b.CreateZExtOrTrunc(v, a.bwin);
        nv = b.CreateAnd(b.CreateShl(nv, a.bshift), mask);
        llvm::Value *w = b.CreateAlignedLoad(a.bwin, a.v, llvm::Align(1));
        w = b.CreateOr(b.CreateAnd(w, ~mask), nv);
        b.CreateAlignedStore(w, a.v, llvm::Align(1));
    }

    // after a return/break the current block is closed; later code goes to an unreachable block
    void ensure_open() {
        if (b.GetInsertBlock()->getTerminator()) b.SetInsertPoint(BasicBlock::Create(ctx, "dead", fn));
    }
    bool open() const { return !b.GetInsertBlock()->getTerminator(); }

    Val lookup_local(const std::string &n) {
        for (auto s = scopes.rbegin(); s != scopes.rend(); ++s) {
            auto it = s->vars.find(n);
            if (it != s->vars.end()) {
                s->used.insert(n);
                return it->second;
            }
        }
        return {};
    }

    Global *lookup_global(Form *sym) {
        std::string k = global_key(sym);
        return k.empty() ? nullptr : &c.globals_[k];
    }

    // does f's value take its type from the context? (literals and arithmetic on them)
    bool untyped(Form *f) {
        f = expand(f);
        if (f->kind == Form::Int || f->kind == Form::Float) return true;
        if (f->kind == Form::Sym) {
            if (lookup_local(key(f)).v) return false;
            Global *g = lookup_global(f);
            return g && g->kind == Global::Const && g->form && untyped(g->form);
        }
        if (f->is_list() && f->size() >= 2 && is_arith(f->head())) {
            for (size_t i = 1; i < f->size(); i++)
                if (!untyped(f->at(i))) return false;
            return true;
        }
        if (f->is_call("if") && f->size() == 4) return untyped(f->at(2)) && untyped(f->at(3));  // (if c 1.0 -1.0)
        return f->is_call("sizeof") || f->is_call("alignof");  // constants, like literals
    }
    // the type an untyped form gets when nothing is expected
    Type *default_type(Form *f) {
        f = expand(f);
        if (f->kind == Form::Float) return t_float(64);
        if (f->kind == Form::Int) {
            int64_t v = (int64_t)f->ival;
            bool hex = f->text.find("0x") != std::string::npos || f->text.find("0X") != std::string::npos;
            if (v >= INT32_MIN && v <= INT32_MAX) return t_int(32, true);
            if (hex && f->ival <= UINT32_MAX) return t_int(32, false);
            if (hex && (int64_t)f->ival < 0) return t_int(64, false);
            return t_int(64, true);
        }
        if (f->kind == Form::Sym) return default_type(lookup_global(f)->form);
        if (f->is_call("sizeof") || f->is_call("alignof")) return t_int(64, false);
        if (f->is_call("if")) return default_type(mk_list(f->loc, {mk_sym(f->loc, "+"), f->at(2), f->at(3)}));
        Type *t = t_int(32, true);
        for (size_t i = 1; i < f->size(); i++) {
            Type *u = default_type(f->at(i));
            if (u->is_float() || (u->bits > t->bits)) t = u;
        }
        return t;
    }

    llvm::Value *to_bool(Val v, Loc at) {
        switch (v.t->kind) {
        case Type::Bool: return v.v;
        case Type::Int: return b.CreateICmpNE(v.v, llvm::ConstantInt::get(ll(v.t), 0));
        case Type::Ptr: return b.CreateIsNotNull(v.v);
        default: fatal(at, "%s is not a condition", type_name(v.t).c_str());
        }
    }

    // implicit conversions on assignment and argument passing: same type, or
    // between (ptr void) and any pointer
    // Lossless widening, done implicitly (as Zig does): a smaller integer into a
    // bigger one of the same signedness, or an unsigned one into a bigger signed one
    static bool widens(Type *from, Type *to) {
        return from->is_int() && to->is_int() && !from->en && !to->en && to->bits > from->bits && (from->sign == to->sign || to->sign);
    }
    // two operands of different integer types: both get the wider type, if one widens to the other
    bool unify(Val &a, Val &x, Loc at) {
        if (widens(a.t, x.t)) a = cast(a, x.t, at);
        else if (widens(x.t, a.t)) x = cast(x, a.t, at);
        return a.t == x.t;
    }

    Val coerce(Val v, Type *to, Loc at, const char *what) {
        if (v.t == to) return v;
        if (v.t->is_ptr() && to->is_ptr() && (v.t->elem == t_void() || to->elem == t_void())) return {v.v, to};
        if (widens(v.t, to) || (v.t->is_float() && to->is_float() && to->bits > v.t->bits)) return cast(v, to, at);
        if (v.t == t_str() && to->is_ptr()) fatal(at, "%s: a str is not a C string (no terminator): use a c\"...\" literal, or s.data with a terminator you add", what);
        type_error(at, what, to, v.t);
    }

    // an expression where a value of type `want` goes (assignment, argument,
    // return): arrays used as pointers decay to their first element
    Val value_for(Form *f, Type *want, const char *what) {
        if (want && want->is_ptr() && is_place(f)) {
            Val a = place(f);
            if (a.t->kind == Type::Array && (a.t->elem == want->elem || want->elem == t_void())) return {a.v, want};
        }
        Val v = expr(f, want);
        return want ? coerce(v, want, f->loc, what) : v;
    }

    /* ------------------------------------------------------- places ---- */

    bool is_place(Form *f) {
        f = expand(f);
        if (f->kind == Form::Sym) {
            if (lookup_local(key(f)).v) return true;
            Global *g = lookup_global(f);
            return g && g->kind == Global::Var;
        }
        return f->is_call(".") || f->is_call("aref") || f->is_call("deref");
    }

    // the address of a place, with the place's type
    Val place(Form *f) {
        f = expand(f);
        if (f->kind == Form::Sym) {
            Val l = lookup_local(key(f));
            if (l.v) return l;
            Global *g = lookup_global(f);
            if (g && g->kind == Global::Var) return {g->value, g->type};
            fatal(f->loc, "'%s' is not a variable", f->text.c_str());
        }
        if (f->is_call(".")) {
            if (f->size() < 3) fatal(f->loc, "(. x field ...) needs a field");
            Val a = struct_base(f->at(1));
            for (size_t i = 2; i < f->size(); i++) a = field_addr(a, f->at(i));
            return a;
        }
        if (f->is_call("aref")) {
            if (f->size() != 3) fatal(f->loc, "(aref a i)");
            Val base;
            llvm::Value *len = nullptr;  // the length, where there is one (for --checks)
            if (is_place(f->at(1))) {
                base = place(f->at(1));
                if (base.t == t_str() || (base.t->kind == Type::Struct && base.t->st->slice_of)) {
                    if (c.checks) len = load(field_addr(base, mk_sym(f->loc, "len")));
                    base = field_addr(base, mk_sym(f->loc, "data"));  // a str's bytes, a slice's elements
                }
                if (base.t->kind == Type::Ptr) base = {load(base), base.t};
            } else {
                base = expr(f->at(1), nullptr);
                if (base.t == t_str() || (base.t->kind == Type::Struct && base.t->st->slice_of)) {
                    if (c.checks) len = b.CreateExtractValue(base.v, 1);
                    base = {b.CreateExtractValue(base.v, 0), base.t->st->fields[0].type};
                }
                if (base.t->kind == Type::Array) base = spill(base);
            }
            Val i = expr(f->at(2), t_int(64, true));
            if (!i.t->is_int()) fatal(f->at(2)->loc, "index must be an integer, got %s", type_name(i.t).c_str());
            llvm::Value *idx = b.CreateIntCast(i.v, b.getInt64Ty(), i.t->sign);
            if (base.t->kind == Type::Array) len = b.getInt64(base.t->n);
            if (len) check(b.CreateICmpULT(idx, len), f->at(2)->loc, "index %ld out of bounds (length %lu)", {idx, len});
            if (base.t->kind == Type::Array) {
                llvm::Value *p = b.CreateInBoundsGEP(ll(base.t), base.v, {b.getInt64(0), idx});
                return {p, base.t->elem};
            }
            if (base.t->kind == Type::Ptr) {
                if (base.t->elem == t_void()) fatal(f->loc, "cannot index a (ptr void)");
                return {b.CreateInBoundsGEP(ll(base.t->elem), base.v, idx), base.t->elem};
            }
            fatal(f->at(1)->loc, "cannot index %s", type_name(base.t).c_str());
        }
        if (f->is_call("deref")) {
            if (f->size() != 2) fatal(f->loc, "(deref p)");
            Val p = expr(f->at(1), nullptr);
            if (!p.t->is_ptr() || p.t->elem == t_void()) fatal(f->loc, "cannot dereference %s", type_name(p.t).c_str());
            return {p.v, p.t->elem};
        }
        fatal(f->loc, "%s is not a place", show(f).c_str());
    }

    // Temporaries used up at once (make, struct arguments and results) borrow
    // a stack slot from a per-function pool and give it back, so a function
    // making thousands of struct values doesn't get thousands of slots.
    std::map<Type *, std::vector<llvm::AllocaInst *>> temp_pool;
    llvm::AllocaInst *temp(Type *t) {
        auto &free = temp_pool[t];
        if (free.empty()) return alloca_(t, "tmp");
        llvm::AllocaInst *a = free.back();
        free.pop_back();
        return a;
    }
    void done(Type *t, llvm::AllocaInst *a) { temp_pool[t].push_back(a); }

    // a value in memory, for taking addresses of temporaries
    Val spill(Val v) {
        llvm::AllocaInst *a = alloca_(v.t, "tmp");
        store({a, v.t}, v.v);
        return {a, v.t};
    }

    // the address of x for field access: x itself if it is a place, what it
    // points to if it is a pointer value, or a temporary copy
    Val struct_base(Form *x) {
        if (is_place(x)) return place(x);
        Val v = expr(x, nullptr);
        if (v.t->is_ptr()) return {v.v, v.t->elem};
        return spill(v);
    }

    Val field_addr(Val a, Form *name) {
        while (a.t->is_ptr()) a = {load(a), a.t->elem};  // auto-deref
        if (a.t->kind != Type::Struct) fatal(name->loc, "%s has no field '%s'", type_name(a.t).c_str(), name->text.c_str());
        ensure_layout(a.t->st, name->loc);
        const Field *fd = a.t->st->find(name->text);
        if (!fd) fatal(name->loc, "%s has no field '%s'", type_name(a.t).c_str(), name->text.c_str());
        if (fd->bit_width) {
            uint64_t first = fd->bit_off / 8, last = (fd->bit_off + fd->bit_width - 1) / 8;
            Val v{at_offset(a.v, first), fd->type, 1};
            v.bwin = b.getIntNTy(8 * (last - first + 1));
            v.bshift = fd->bit_off % 8;
            v.bwidth = fd->bit_width;
            return v;
        }
        llvm::Value *p = fd->offset ? b.CreateInBoundsGEP(b.getInt8Ty(), a.v, b.getInt64(fd->offset)) : a.v;
        // alignment: what the struct's alignment and the offset guarantee
        uint64_t base = a.align ? a.align : align_of(a.t);
        uint64_t al = fd->offset ? (fd->offset & -fd->offset) : base;
        if (al > base) al = base;
        if (al > align_of(fd->type)) al = align_of(fd->type);
        return {p, fd->type, al};
    }

    /* --------------------------------------------------- expressions --- */

    Val literal(Form *f, Type *want) {
        if (f->kind == Form::Int) {
            if (!want || (!want->is_num() && !want->is_ptr())) want = default_type(f);
            if (want->is_float()) return float_const(f->text, want);
            if (want->is_ptr()) {
                if (f->ival != 0) fatal(f->loc, "only 0 converts to a pointer");
                return {llvm::ConstantPointerNull::get(b.getPtrTy()), want};
            }
            if (want->bits < 64) {
                int64_t v = (int64_t)f->ival;
                bool fits = want->sign ? (v >= -(int64_t(1) << (want->bits - 1)) && v < (int64_t(1) << (want->bits - 1)))
                                       : (f->ival < (uint64_t(1) << want->bits)) || (v < 0 && v >= -(int64_t(1) << (want->bits - 1)));
                if (!fits) fatal(f->loc, "%s does not fit in %s", f->text.c_str(), type_name(want).c_str());
            }
            return {llvm::ConstantInt::get(ll(want), f->ival), want};
        }
        // Float
        if (want && want->is_int()) fatal(f->loc, "%s is not an integer", f->text.c_str());
        return float_const(f->text, want && want->is_float() ? want : t_float(64));
    }

    // "..." is a str: exactly its bytes and their count. c"..." is a C string:
    // the bytes and a terminating zero, a (ptr i8) (or (ptr u8), (ptr void)
    // where one is wanted) — the only kind of literal C functions get.
    std::map<std::pair<std::string, bool>, llvm::GlobalVariable *> strings;  // one copy of each literal
    Val string(Form *f, Type *want) {
        llvm::GlobalVariable *&g = strings[{f->text, f->cstr}];
        if (!g) {
            llvm::Constant *bytes = llvm::ConstantDataArray::getString(ctx, f->text, f->cstr);
            g = new llvm::GlobalVariable(mod, bytes->getType(), true, llvm::GlobalValue::PrivateLinkage, bytes, f->cstr ? "cstr" : "str");
            g->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
            g->setAlignment(llvm::Align(1));
        }
        bool byte_ptr = want && want->is_ptr() && (want->elem == t_void() || (want->elem->is_int() && want->elem->bits == 8));
        if (f->cstr) return {g, byte_ptr ? want : t_ptr(t_int(8, true))};
        if (byte_ptr) fatal(f->loc, "a str is not a C string: write this literal as c\"...\" (or pass s.len and s.data)");
        auto *st = llvm::cast<llvm::StructType>(ll(t_str()));
        llvm::Constant *parts[] = {g, llvm::ConstantInt::get(b.getInt64Ty(), f->text.size())};
        return {llvm::ConstantStruct::get(st, parts), t_str()};
    }
    // a C-string literal, an if choosing between two, or a constant naming one
    bool is_cstring_lit(Form *f) {
        f = expand(f);
        if (f->kind == Form::Sym && !lookup_local(key(f)).v) {
            Global *g = lookup_global(f);
            return g && g->kind == Global::Const && g->form && is_cstring_lit(g->form);
        }
        return (f->kind == Form::Str && f->cstr) ||
               (f->is_call("if") && f->size() == 4 && is_cstring_lit(f->at(2)) && is_cstring_lit(f->at(3)));
    }

    // rounded once, from the text: like C's 1.5f / 1.5
    Val float_const(const std::string &text, Type *t) {
        if (t->bits == 32) return {llvm::ConstantFP::get(b.getFloatTy(), strtof(text.c_str(), nullptr)), t};
        return {llvm::ConstantFP::get(b.getDoubleTy(), strtod(text.c_str(), nullptr)), t};
    }

    Val symbol(Form *f, Type *want) {
        const std::string &n = f->text;
        if (n == "true" || n == "false") return {b.getInt1(n == "true"), t_bool()};
        if (n == "null") {  // the zero value of the wanted type; a (ptr void) without one
            Type *t = want && want != t_void() && want->kind != Type::Fn ? want : t_ptr(t_void());
            return {llvm::Constant::getNullValue(ll(t)), t};
        }
        if (n == "undefined") {  // no value at all: the memory is left as it is
            if (!want || want == t_void()) fatal(f->loc, "undefined needs a type: (x T undefined)");
            return {llvm::UndefValue::get(ll(want)), want};
        }
        Val l = lookup_local(key(f));
        if (l.v) return {load(l), l.t};
        Global *g = lookup_global(f);
        if (!g) fatal(f->loc, "unknown name '%s'", n.c_str());
        switch (g->kind) {
        case Global::Var: return {load({g->value, g->type}), g->type};
        case Global::Func: return {g->value, t_ptr(g->type)};
        case Global::Const:
            if (g->form) return expr(g->form, want);
            return {g->value, g->type};
        }
        return {};
    }

    Val expr(Form *f, Type *want) {
        f = expand(f);
        if (dsp && f->kind == Form::List) set_loc(f->loc);
        switch (f->kind) {
        case Form::Int: case Form::Float: return literal(f, want);
        case Form::Str: return string(f, want);
        case Form::Sym: return symbol(f, want);
        case Form::Fn: fatal(f->loc, "a compile-time function (from a macro) cannot be used in code");
        case Form::List: break;
        }
        if (f->items.empty()) fatal(f->loc, "empty form ()");
        const char *h = f->head();
        if (!*h) return call(f);
        std::string hs = h;
        if (is_arith(h)) return arith(f, want);
        if (is_cmp(h)) return compare(f);
        if (hs == "do") return body(f, 1, want);
        if (hs == "let") return let(f, want);
        if (hs == "set") return set(f);
        if (hs.size() >= 2 && hs.back() == '=' && hs != "=" && hs != "!=" && hs != "<=" && hs != ">=" &&
            is_arith(hs.substr(0, hs.size() - 1).c_str()))
            return compound(f, hs.substr(0, hs.size() - 1));
        if (hs.compare(0, 7, "atomic-") == 0 || hs == "fence") return atomic(f, hs);
        if (hs == "if") return if_(f, want);
        if (hs == "when" || hs == "unless") return when(f, hs == "unless");
        if (hs == "while") return while_(f);
        if (hs == "for") return for_(f);
        if (hs == "break" || hs == "continue") return jump(f, hs == "break");
        if (hs == "return") return return_(f);
        if (hs == "and" || hs == "or") return logic(f, hs == "and");
        if (hs == "not") {
            if (f->size() != 2) fatal(f->loc, "(not x)");
            return {b.CreateNot(to_bool(expr(f->at(1), t_bool()), f->loc)), t_bool()};
        }
        if (hs == "~") {
            if (f->size() != 2) fatal(f->loc, "(~ x)");
            Val v = expr(f->at(1), want);
            if (!v.t->is_int()) fatal(f->loc, "~ needs an integer");
            return {b.CreateNot(v.v), v.t};
        }
        if (hs == "." && f->size() == 3 && f->at(1)->kind == Form::Sym && f->at(2)->kind == Form::Sym && !is_place(f->at(1)) &&
            !lookup_global(f->at(1))) {
            std::string tk = type_key(f->at(1));
            if (!tk.empty() && c.types_[tk]->en) return enum_member(c.types_[tk], f->at(2));
        }
        if (hs == "." || hs == "aref" || hs == "deref") {
            Val a = place(f);
            if (a.t->kind == Type::Array && want && want->is_ptr()) return {a.v, want};
            return {load(a), a.t};
        }
        if (hs == "addr") {
            if (f->size() != 2) fatal(f->loc, "(addr place)");
            if (f->at(1)->kind == Form::Sym) {
                Global *g = lookup_global(f->at(1));
                if (g && g->kind == Global::Func && !lookup_local(key(f->at(1))).v) return {g->value, t_ptr(g->type)};
            }
            Val a = place(f->at(1));
            if (a.bwidth) fatal(f->at(1)->loc, "cannot take the address of a bit field");
            return {a.v, t_ptr(a.t)};
        }
        if (hs == "truncate") {  // (truncate T e): the low bits of an integer, on purpose (never checked)
            if (f->size() != 3) fatal(f->loc, "(truncate T e)");
            Type *to = parse_type(f->at(1));
            Val v = expr(f->at(2), nullptr);
            if (!v.t->is_int() || !to->is_int()) fatal(f->loc, "truncate is for integers: %s to %s", type_name(v.t).c_str(), type_name(to).c_str());
            return {b.CreateIntCast(v.v, ll(to), v.t->sign), to};
        }
        if (hs == "cast" || hs == "bitcast") {
            if (f->size() != 3) fatal(f->loc, "(%s T e)", h);
            Type *to = parse_type(f->at(1));
            // the operand keeps its own type (3.99 is an f64 here), then converts
            Val v = expr(f->at(2), nullptr);
            return hs == "cast" ? cast(v, to, f->loc) : bitcast(v, to, f->loc);
        }
        if (hs == "sizeof" || hs == "alignof") {
            if (f->size() != 2) fatal(f->loc, "(%s T)", h);
            Type *t = parse_type(f->at(1));
            if (t->kind == Type::Struct) ensure_layout(t->st, f->loc);
            Type *rt = want && want->is_int() ? want : t_int(64, false);
            return {llvm::ConstantInt::get(ll(rt), hs == "sizeof" ? size_of(t) : align_of(t)), rt};
        }
        if (hs == "make") return make(f);
        if (hs == "defer") {  // (defer e ...): run when this block exits, however it exits
            if (scopes.empty()) fatal(f->loc, "defer outside a function");
            if (in_defer) fatal(f->loc, "defer inside a defer");
            std::vector<Form *> items = f->items;
            items[0] = mk_sym(f->loc, "do");
            scopes.back().defers.push_back(mk_list(f->loc, items));
            return {nullptr, t_void()};
        }
        if (hs == "case") return case_(f, want);
        if (hs == "cond") return cond(f, want);
        if (hs == "slice-of") return slice_of(f);
        if (hs == "using") {  // (using m ...) in a block: m's names unqualified until the block ends
            if (scopes.empty()) fatal(f->loc, "using outside a function");
            for (size_t i = 1; i < f->size(); i++) {
                if (f->at(i)->kind != Form::Sym) fatal(f->at(i)->loc, "(using module ...)");
                if (!aliased(cur, f->at(i)->text)) fatal(f->at(i)->loc, "no module '%s' is imported here", f->at(i)->text.c_str());
                scopes.back().usings.push_back(f->at(i)->text);
            }
            return {nullptr, t_void()};
        }
        if (hs == "defined") {  // (defined NAME): is there a global NAME (e.g. from -D)?
            if (f->size() != 2 || f->at(1)->kind != Form::Sym) fatal(f->loc, "(defined NAME)");
            return {b.getInt1(!global_key(f->at(1)).empty()), t_bool()};
        }
        if (hs == "if-defined") {  // compile-time: only the chosen branch is compiled, like #ifdef
            if (f->size() < 3 || f->size() > 4 || f->at(1)->kind != Form::Sym) fatal(f->loc, "(if-defined NAME then [else])");
            if (!global_key(f->at(1)).empty()) return expr(f->at(2), want);
            return f->size() == 4 ? expr(f->at(3), want) : Val{nullptr, t_void()};
        }
        if (hs == "quote" || hs == "quasiquote") fatal(f->loc, "quoted forms exist only inside macros");
        return call(f);
    }

    // warnings for a scope that ends: its variables nobody read
    void end_scope() {
        Scope &s = scopes.back();
        for (Form *name : s.declared)
            if (!s.used.count(key(name)) && name->text[0] != '_' && !name->mark && !name->loc.via)
                warn(name->loc, "'%s' is never used", name->text.c_str());
        scopes.pop_back();
    }

    // a block: its own scope (for using and defer); the last form is its value,
    // computed before the block's defers run
    Val body(Form *f, size_t from, Type *want) {
        scopes.emplace_back();
        Val last{nullptr, t_void()};
        bool warned = false;
        for (size_t i = from; i < f->size(); i++) {
            if (!open() && !warned && !f->at(i)->loc.via) {  // after a return, break or continue
                warn(f->at(i)->loc, "unreachable code");
                warned = true;
            }
            ensure_open();
            last = expr(f->at(i), i + 1 == f->size() ? want : nullptr);
        }
        if (open()) run_defers(scopes.size() - 1);
        end_scope();
        return last;
    }
    // one form as a block (an if branch)
    Val block(Form *f, Type *want) {
        scopes.emplace_back();
        Val v = expr(f, want);
        if (open()) run_defers(scopes.size() - 1);
        scopes.pop_back();
        return v;
    }

    // the defers of scopes[from..], innermost first, each compiled with the
    // scopes it was written in (a later local of the same name can't capture it)
    bool in_defer = false;
    void run_defers(size_t from) {
        for (size_t k = scopes.size(); k-- > from;) {
            std::vector<Form *> ds = scopes[k].defers;
            if (ds.empty()) continue;
            std::vector<Scope> inner(scopes.begin() + k + 1, scopes.end());
            scopes.resize(k + 1);
            bool saved = in_defer;
            in_defer = true;
            for (size_t i = ds.size(); i-- > 0;) {
                ensure_open();
                expr(ds[i], nullptr);
            }
            in_defer = saved;
            scopes.insert(scopes.end(), inner.begin(), inner.end());
        }
    }

    // the operand type of an arithmetic or compare form: the first typed operand's
    Type *operand_type(Form *f, size_t from, Type *want, size_t *typed_at) {
        for (size_t i = from; i < f->size(); i++)
            if (!untyped(f->at(i))) { *typed_at = i; return nullptr; }
        *typed_at = 0;
        if (want && want->is_num()) return want;
        Type *t = t_int(32, true);
        for (size_t i = from; i < f->size(); i++) {
            Type *u = default_type(f->at(i));
            if (u->is_float() && !t->is_float()) t = u;
            else if (u->kind == t->kind && u->bits > t->bits) t = u;
        }
        return t;
    }

    // operands of f from `from` on, all of one type
    // (compare: the operands of a comparison, which all take the first's type;
    // arithmetic on a pointer takes integer offsets instead)
    std::vector<Val> operands(Form *f, size_t from, Type *want, bool compare = false) {
        size_t k;
        Type *t = operand_type(f, from, want, &k);
        std::vector<Val> vs(f->size());
        if (!t) {
            vs[k] = expr(f->at(k), nullptr);
            t = vs[k].t;
        }
        for (size_t i = from; i < f->size(); i++) {
            if (vs[i].v) continue;
            Type *ti = t->is_ptr() && !compare ? t_int(64, true) : t;  // pointer arithmetic: integer offsets
            vs[i] = expr(f->at(i), ti);
            if (t->is_ptr() || vs[i].t == t) continue;
            if (widens(vs[i].t, t)) vs[i] = cast(vs[i], t, f->at(i)->loc);
            else if (widens(t, vs[i].t)) {  // a wider operand: everything so far widens to it
                t = vs[i].t;
                for (Val &v : vs)
                    if (v.v && v.t != t) v = cast(v, t, f->at(i)->loc);
            } else type_error(f->at(i)->loc, f->head(), t, vs[i].t);
        }
        return {vs.begin() + from, vs.end()};
    }

    /* ------------------------------------------- operator overloading --- */

    // an operand: compiled (v.v set) or still untyped (only the form)
    struct Opnd {
        Val v;
        Form *f;
    };

    // typed operands compiled in order; untyped ones wait for a type
    std::vector<Opnd> opnds(Form *f) {
        std::vector<Opnd> xs;
        for (size_t i = 1; i < f->size(); i++) xs.push_back({untyped(f->at(i)) ? Val{} : expr(f->at(i), nullptr), f->at(i)});
        return xs;
    }

    // the defop matching these operands, called; found = false if none matches
    Val overload(const std::string &op, std::vector<Opnd> xs, Loc at, bool *found) {
        std::vector<Global *> match;
        for (Global *g : overloads[op]) {
            if (g->type->params.size() != xs.size()) continue;
            bool ok = true;
            for (size_t i = 0; i < xs.size() && ok; i++)
                ok = xs[i].v.v ? g->type->params[i] == xs[i].v.t : g->type->params[i]->is_num();
            if (ok) match.push_back(g);
        }
        *found = !match.empty();
        if (match.empty()) return {};
        if (match.size() > 1) {
            // a float literal prefers a float parameter, an integer literal an integer one
            std::vector<Global *> pref;
            for (Global *g : match) {
                bool ok = true;
                for (size_t i = 0; i < xs.size() && ok; i++)
                    if (!xs[i].v.v) ok = default_type(xs[i].f)->is_float() == g->type->params[i]->is_float();
                if (ok) pref.push_back(g);
            }
            if (!pref.empty()) match = pref;
        }
        if (match.size() > 1) fatal(at, "(%s ...) matches %zu definitions; give the literals a type", op.c_str(), match.size());
        Global *g = match[0];
        std::vector<Val> args;
        for (size_t i = 0; i < xs.size(); i++) args.push_back(xs[i].v.v ? xs[i].v : expr(xs[i].f, g->type->params[i]));
        return emit_call(g->value, g->type, args, {});
    }

    // (op a b) with built-in semantics, for operands already sorted out
    Opnd builtin_pair(const std::string &op, Opnd a, Opnd x, Loc at, bool is_compare) {
        if (!a.v.v && !x.v.v && !is_compare)  // both untyped: stays untyped
            return {Val{}, mk_list(at, {mk_sym(at, op), a.f, x.f})};
        Type *t = a.v.v ? a.v.t : x.v.t;
        if (!a.v.v) a.v = expr(a.f, t);
        if (!x.v.v) x.v = expr(x.f, t->is_ptr() && !is_compare ? t_int(64, true) : t);
        if (is_compare) return {compare_vals(op, a.v, x.v, at), nullptr};
        bool shift = op == "<<" || op == ">>";  // as in C, the count may be any integer type
        if (!t->is_ptr() && a.v.t != x.v.t && !(shift && a.v.t->is_int() && x.v.t->is_int()) && !unify(a.v, x.v, at))
            type_error(at, op.c_str(), a.v.t, x.v.t);
        if (!t->is_num() && !t->is_ptr()) fatal(at, "no (%s %s %s): define it with defop", op.c_str(), type_name(a.v.t).c_str(), type_name(x.v.t).c_str());
        return {binop(op, a.v, x.v, at), nullptr};
    }

    Val arith(Form *f, Type *want) {
        std::string op = f->head();
        if (f->size() < 2) fatal(f->loc, "(%s ...) needs operands", op.c_str());
        if ((op == "<<" || op == ">>") && f->size() == 3 && !overloads.count(op)) {
            // as in C, the shift count may be any integer type
            Val a = expr(f->at(1), want);
            Val n = expr(f->at(2), untyped(f->at(2)) ? a.t : nullptr);
            return binop(op, a, n, f->at(2)->loc);
        }
        if (overloads.count(op) && !untyped(f)) {  // all-literal arithmetic is never overloaded
            std::vector<Opnd> xs = opnds(f);
            bool found;
            if (xs.size() == 1) {
                Val r = overload(op, xs, f->loc, &found);
                if (found) return r;
            } else {
                Opnd acc = xs[0];
                for (size_t i = 1; i < xs.size(); i++) {
                    Val r = overload(op, {acc, xs[i]}, f->at(i + 1)->loc, &found);
                    acc = found ? Opnd{r, nullptr} : builtin_pair(op, acc, xs[i], f->at(i + 1)->loc, false);
                }
                return acc.v.v ? acc.v : expr(acc.f, want);
            }
            // a unary operator with no matching definition: built in
            Val v = xs[0].v.v ? xs[0].v : expr(xs[0].f, want);
            if (op != "-") fatal(f->loc, "(%s x) needs two operands", op.c_str());
            if (v.t->is_float()) return {b.CreateFNeg(v.v), v.t};
            if (v.t->is_int()) return {b.CreateNeg(v.v), v.t};
            fatal(f->loc, "no (- %s): define it with defop", type_name(v.t).c_str());
        }
        std::vector<Val> vs = operands(f, 1, want);
        if (vs.size() == 1) {
            if (op != "-") fatal(f->loc, "(%s x) needs two operands", op.c_str());
            Val v = vs[0];
            if (v.t->is_float()) return {b.CreateFNeg(v.v), v.t};
            if (v.t->is_int()) return {b.CreateNeg(v.v), v.t};
            fatal(f->loc, "cannot negate %s", type_name(v.t).c_str());
        }
        Val acc = vs[0];
        for (size_t i = 1; i < vs.size(); i++) acc = binop(op, acc, vs[i], f->at(i + 1)->loc);
        return acc;
    }

    Val binop(const std::string &op, Val a, Val x, Loc at) {
        Type *t = a.t;
        if (t->is_ptr()) {
            if (op == "+" || op == "-") {
                if (x.t->is_int()) {
                    if (t->elem == t_void()) fatal(at, "arithmetic on (ptr void)");
                    llvm::Value *i = b.CreateIntCast(x.v, b.getInt64Ty(), x.t->sign);
                    if (op == "-") i = b.CreateNeg(i);
                    return {b.CreateGEP(ll(t->elem), a.v, i), t};
                }
                if (op == "-" && x.t == t) {
                    llvm::Value *d = b.CreateSub(b.CreatePtrToInt(a.v, b.getInt64Ty()), b.CreatePtrToInt(x.v, b.getInt64Ty()));
                    return {b.CreateExactSDiv(d, b.getInt64(size_of(t->elem))), t_int(64, true)};
                }
            }
            fatal(at, "(%s %s %s) is not defined", op.c_str(), type_name(t).c_str(), type_name(x.t).c_str());
        }
        if (op == "<<" || op == ">>") {
            if (!t->is_int() || !x.t->is_int()) fatal(at, "shifts need integers");
            llvm::Value *n = b.CreateIntCast(x.v, ll(t), false);
            if (op == "<<") return {b.CreateShl(a.v, n), t};
            return {t->sign ? b.CreateAShr(a.v, n) : b.CreateLShr(a.v, n), t};
        }
        if (t->is_float()) {
            if (op == "+") return {b.CreateFAdd(a.v, x.v), t};
            if (op == "-") return {b.CreateFSub(a.v, x.v), t};
            if (op == "*") return {b.CreateFMul(a.v, x.v), t};
            if (op == "/") return {b.CreateFDiv(a.v, x.v), t};
            if (op == "%") return {b.CreateFRem(a.v, x.v), t};
            fatal(at, "'%s' needs integers", op.c_str());
        }
        if (!t->is_int()) fatal(at, "no (%s %s %s): define it with defop", op.c_str(), type_name(t).c_str(), type_name(t).c_str());
        if (op == "+") return {b.CreateAdd(a.v, x.v), t};
        if (op == "-") return {b.CreateSub(a.v, x.v), t};
        if (op == "*") return {b.CreateMul(a.v, x.v), t};
        if ((op == "/" || op == "%") && c.checks) {
            check(b.CreateICmpNE(x.v, llvm::ConstantInt::get(ll(t), 0)), at, "division by zero");
            if (t->sign)
                check(b.CreateNot(b.CreateAnd(b.CreateICmpEQ(a.v, llvm::ConstantInt::get(ll(t), llvm::APInt::getSignedMinValue(t->bits))),
                                              b.CreateICmpEQ(x.v, llvm::ConstantInt::getSigned(ll(t), -1)))),
                      at, "integer overflow in division");
        }
        if (op == "/") return {t->sign ? b.CreateSDiv(a.v, x.v) : b.CreateUDiv(a.v, x.v), t};
        if (op == "%") return {t->sign ? b.CreateSRem(a.v, x.v) : b.CreateURem(a.v, x.v), t};
        if (op == "&") return {b.CreateAnd(a.v, x.v), t};
        if (op == "|") return {b.CreateOr(a.v, x.v), t};
        return {b.CreateXor(a.v, x.v), t};
    }

    Val compare(Form *f) {
        std::string op = f->head();
        if (f->size() != 3) fatal(f->loc, "(%s a b)", op.c_str());
        if (overloads.count(op)) {
            std::vector<Opnd> xs = opnds(f);
            bool found;
            Val r = overload(op, xs, f->loc, &found);
            if (found) return r;
            if (!xs[0].v.v && !xs[1].v.v) xs[0].v = expr(xs[0].f, default_type(mk_list(f->loc, {mk_sym(f->loc, "+"), xs[0].f, xs[1].f})));
            return builtin_pair(op, xs[0], xs[1], f->loc, true).v;
        }
        std::vector<Val> vs = operands(f, 1, nullptr, true);
        return compare_vals(op, vs[0], vs[1], f->at(2)->loc);
    }

    Val compare_vals(const std::string &op, Val a, Val x, Loc at) {
        if (x.t != a.t) {
            if (a.t->is_ptr() && x.t->is_ptr()) x.t = a.t;
            else if (!unify(a, x, at)) type_error(at, op.c_str(), a.t, x.t);
        }
        Type *t = a.t;
        using P = llvm::CmpInst::Predicate;
        int k = op == "=" ? 0 : op == "!=" ? 1 : op == "<" ? 2 : op == "<=" ? 3 : op == ">" ? 4 : 5;
        if (t->is_float()) {
            const P fp[] = {P::FCMP_OEQ, P::FCMP_UNE, P::FCMP_OLT, P::FCMP_OLE, P::FCMP_OGT, P::FCMP_OGE};
            return {b.CreateFCmp(fp[k], a.v, x.v), t_bool()};
        }
        if (t->kind == Type::Struct || t->kind == Type::Array) fatal(at, "no (%s %s %s): define it with defop", op.c_str(), type_name(t).c_str(), type_name(t).c_str());
        const P si[] = {P::ICMP_EQ, P::ICMP_NE, P::ICMP_SLT, P::ICMP_SLE, P::ICMP_SGT, P::ICMP_SGE};
        const P ui[] = {P::ICMP_EQ, P::ICMP_NE, P::ICMP_ULT, P::ICMP_ULE, P::ICMP_UGT, P::ICMP_UGE};
        return {b.CreateICmp(t->is_int() && t->sign ? si[k] : ui[k], a.v, x.v), t_bool()};
    }

    Val logic(Form *f, bool is_and) {
        if (f->size() < 2) return {b.getInt1(is_and), t_bool()};
        BasicBlock *done = BasicBlock::Create(ctx, is_and ? "and" : "or", fn);
        std::vector<std::pair<llvm::Value *, BasicBlock *>> in;
        for (size_t i = 1; i < f->size(); i++) {
            llvm::Value *v = to_bool(expr(f->at(i), t_bool()), f->at(i)->loc);
            if (i + 1 == f->size()) {
                in.push_back({v, b.GetInsertBlock()});
                b.CreateBr(done);
                break;
            }
            BasicBlock *next = BasicBlock::Create(ctx, "next", fn);
            in.push_back({b.getInt1(!is_and), b.GetInsertBlock()});
            if (is_and) b.CreateCondBr(v, next, done);
            else b.CreateCondBr(v, done, next);
            b.SetInsertPoint(next);
        }
        b.SetInsertPoint(done);
        llvm::PHINode *phi = b.CreatePHI(b.getInt1Ty(), in.size());
        for (auto &p : in) phi->addIncoming(p.first, p.second);
        return {phi, t_bool()};
    }

    Val let(Form *f, Type *want) {
        if (f->size() < 2 || !f->at(1)->is_list()) fatal(f->loc, "(let ((name [T] [init]) ...) body...)");
        scopes.emplace_back();
        for (Form *bd : f->at(1)->items) {
            if (!bd->is_list() || bd->size() < 2 || bd->size() > 3 || bd->at(0)->kind != Form::Sym)
                fatal(bd->loc, "binding must be (name init), (name T init) or (name T)");
            const std::string &name = bd->at(0)->text;
            Type *t = nullptr;
            Form *init = nullptr;
            if (bd->size() == 3) { t = parse_type(bd->at(1)); init = bd->at(2); }
            else if (is_type_form(bd->at(1))) t = parse_type(bd->at(1));
            else init = bd->at(1);
            Val v;
            bool undef = init && init->is_sym("undefined");
            if (undef && !t) fatal(init->loc, "undefined needs a type: (%s T undefined)", name.c_str());
            if (init && !undef) {
                v = value_for(init, t, "initializer");
                t = v.t;
                if (t == t_void()) fatal(init->loc, "'%s' would have type void", name.c_str());
            }
            llvm::AllocaInst *a = alloca_(t, name);
            // everything starts as zero, unless explicitly undefined
            if (!undef) store({a, t}, init ? v.v : llvm::Constant::getNullValue(ll(t)));
            scopes.back()[key(bd->at(0))] = {a, t};
            scopes.back().declared.push_back(bd->at(0));
            if (dsp) declare_var(a, name, t, bd->at(0)->loc);
        }
        Val r = body(f, 2, want);
        end_scope();
        return r;
    }

    // (x T) in a let: is the second item a type rather than an initializer?
    bool is_type_form(Form *f) {
        f = expand(f);
        if (f->kind == Form::Sym) {
            static const char *const names[] = {"void", "bool", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "isize", "usize", "f32", "f64", "str"};
            for (const char *n : names)
                if (f->text == n) return true;
            if (tbind.count(f->text) && !lookup_local(key(f)).v) return true;
            return !type_key(f).empty() && !lookup_local(key(f)).v && !lookup_global(f);
        }
        if (f->is_list() && f->size() >= 2 && !generic_key(f->at(0), generic_structs).empty()) return true;
        return f->is_call("ptr") || f->is_call("array") || f->is_call("fn") || f->is_call("slice");
    }

    Val set(Form *f) {
        if (f->size() != 3) fatal(f->loc, "(set place value)");
        Val a = place(f->at(1));
        Val v = value_for(f->at(2), a.t, "assignment");
        store(a, v.v);
        return {nullptr, t_void()};
    }

    // (+= place e) and the other compound assignments: the place's address is
    // computed once; the operator is the built-in one or a defop
    Val compound(Form *f, const std::string &op) {
        if (f->size() != 3) fatal(f->loc, "(%s= place value)", op.c_str());
        Val a = place(f->at(1));
        Opnd x{{load(a), a.t}, nullptr};
        Opnd y{untyped(f->at(2)) ? Val{} : expr(f->at(2), nullptr), f->at(2)};
        Val r;
        bool found = false;
        if (overloads.count(op)) r = overload(op, {x, y}, f->loc, &found);
        if (!found) {
            if (op == "<<" || op == ">>") {  // the count may be any integer type
                if (!y.v.v) y.v = expr(y.f, a.t);
                r = binop(op, x.v, y.v, f->at(2)->loc);
            } else r = builtin_pair(op, x, y, f->at(2)->loc, false).v;
        }
        store(a, coerce(r, a.t, f->at(2)->loc, (op + "=").c_str()).v);
        return {nullptr, t_void()};
    }

    // atomics on a place of integer, pointer (or, for load/store, float) type;
    // an optional last :relaxed :acquire :release :acq-rel :seq-cst (the default)
    Val atomic(Form *f, const std::string &hs) {
        using AO = llvm::AtomicOrdering;
        size_t n = f->size();
        AO order = AO::SequentiallyConsistent;
        Form *last = n > 1 ? f->at(n - 1) : nullptr;
        if (last && last->kind == Form::Sym && last->text[0] == ':') {
            const std::string &o = last->text;
            order = o == ":relaxed" ? AO::Monotonic : o == ":acquire" ? AO::Acquire : o == ":release" ? AO::Release
                  : o == ":acq-rel" ? AO::AcquireRelease : o == ":seq-cst" ? AO::SequentiallyConsistent
                  : (fatal(last->loc, "unknown ordering %s (:relaxed :acquire :release :acq-rel :seq-cst)", o.c_str()), order);
            n--;
        }
        if (hs == "fence") {
            if (n != 1) fatal(f->loc, "(fence [ordering])");
            b.CreateFence(order);
            return {nullptr, t_void()};
        }
        std::string op = hs.substr(7);
        size_t want_args = op == "load" ? 2 : op == "cas" ? 4 : 3;
        if (n != want_args) fatal(f->loc, "(%s place%s [ordering])", hs.c_str(), op == "load" ? "" : op == "cas" ? " expected new" : " value");
        Val a = place(f->at(1));
        if (a.bwidth) fatal(f->at(1)->loc, "a bit field cannot be atomic");
        bool flt_ok = op == "load" || op == "store";
        if (!a.t->is_int() && !a.t->is_ptr() && !(flt_ok && a.t->is_float()))
            fatal(f->at(1)->loc, "%s needs an integer or pointer place, got %s", hs.c_str(), type_name(a.t).c_str());
        llvm::Align al(align_of(a.t));
        if (op == "load") {
            llvm::LoadInst *l = b.CreateAlignedLoad(ll(a.t), a.v, al);
            l->setAtomic(order);
            return {l, a.t};
        }
        Val v = value_for(f->at(2), a.t, "value");
        if (op == "store") {
            llvm::StoreInst *st = b.CreateAlignedStore(v.v, a.v, al);
            st->setAtomic(order);
            return {nullptr, t_void()};
        }
        if (op == "cas") {  // true if the place held `expected` and now holds `new`
            Val nv = value_for(f->at(3), a.t, "value");
            AO fail = order == AO::AcquireRelease ? AO::Acquire : order == AO::Release ? AO::Monotonic : order;
            llvm::Value *r = b.CreateAtomicCmpXchg(a.v, v.v, nv.v, al, order, fail);
            return {b.CreateExtractValue(r, 1), t_bool()};
        }
        using RMW = llvm::AtomicRMWInst::BinOp;
        RMW k = op == "swap" ? RMW::Xchg : op == "add" ? RMW::Add : op == "sub" ? RMW::Sub : op == "and" ? RMW::And
              : op == "or" ? RMW::Or : op == "xor" ? RMW::Xor
              : (fatal(f->loc, "unknown %s (atomic-load store swap add sub and or xor cas, fence)", hs.c_str()), RMW::Xchg);
        if (a.t->is_ptr() && k != RMW::Xchg) fatal(f->loc, "%s needs an integer place", hs.c_str());
        return {b.CreateAtomicRMW(k, a.v, v.v, al, order), a.t};  // the old value
    }

    Val enum_member(Type *t, Form *name) {
        for (auto &v : t->en->values)
            if (v.first == name->text) return {llvm::ConstantInt::get(ll(t), v.second), t};
        fatal(name->loc, "enum %s has no member %s", t->en->name.c_str(), name->text.c_str());
    }

    // (cond (test body...) ... (else body...)): an if / else-if chain
    Val cond(Form *f, Type *want) {
        Form *chain = nullptr;
        for (size_t i = f->size(); i-- > 1;) {
            Form *cl = f->at(i);
            if (!cl->is_list() || cl->items.empty()) fatal(cl->loc, "cond clause must be (test body...)");
            std::vector<Form *> b0 = {mk_sym(cl->loc, "do")};
            b0.insert(b0.end(), cl->items.begin() + 1, cl->items.end());
            Form *bodyf = mk_list(cl->loc, b0);
            if (cl->at(0)->is_sym("else")) {
                if (i + 1 != f->size()) fatal(cl->loc, "else must be the last cond clause");
                chain = bodyf;
            } else {
                std::vector<Form *> it = {mk_sym(cl->loc, "if"), cl->at(0), bodyf};
                if (chain) it.push_back(chain);
                chain = mk_list(cl->loc, it);
            }
        }
        return chain ? expr(chain, want) : Val{nullptr, t_void()};
    }

    // (case x (v body...) ((v1 v2) body...) ... (else body...)): a jump table on an
    // integer or enum; a value if every clause (and an else) gives one of one type
    Val case_(Form *f, Type *want) {
        if (f->size() < 3) fatal(f->loc, "(case x (value body...) ... [(else body...)])");
        Val x = expr(f->at(1), nullptr);
        if (!x.t->is_int()) fatal(f->at(1)->loc, "case needs an integer or enum, got %s", type_name(x.t).c_str());
        BasicBlock *done = BasicBlock::Create(ctx, "endcase", fn), *dflt = done;
        Form *lastcl = f->at(f->size() - 1);
        bool has_else = lastcl->is_list() && !lastcl->items.empty() && lastcl->at(0)->is_sym("else");
        if (has_else) dflt = BasicBlock::Create(ctx, "else", fn);
        llvm::SwitchInst *sw = b.CreateSwitch(x.v, dflt, f->size() - 2);
        std::set<uint64_t> seen;
        std::vector<std::pair<Val, BasicBlock *>> results;
        Type *t = want;
        bool value = has_else;
        for (size_t i = 2; i < f->size(); i++) {
            Form *cl = f->at(i);
            if (!cl->is_list() || cl->size() < 1) fatal(cl->loc, "case clause must be (value body...)");
            bool is_else = cl->at(0)->is_sym("else");
            if (is_else && i + 1 != f->size()) fatal(cl->loc, "else must be the last case clause");
            BasicBlock *bb = is_else ? dflt : BasicBlock::Create(ctx, "when", fn);
            if (!is_else) {
                bool many = cl->at(0)->is_list() && !cl->at(0)->is_call(".");
                std::vector<Form *> vs = many ? cl->at(0)->items : std::vector<Form *>{cl->at(0)};
                for (Form *v : vs) {
                    auto *k = llvm::dyn_cast<llvm::ConstantInt>(constant(v, x.t));
                    if (!k) fatal(v->loc, "a case value must be an integer constant");
                    if (!seen.insert(k->getZExtValue()).second) fatal(v->loc, "%s appears twice in this case", show(v).c_str());
                    sw->addCase(k, bb);
                }
            }
            b.SetInsertPoint(bb);
            Val r = body(cl, 1, t);
            if (!t && r.t != t_void()) t = r.t;
            if (r.t == t_void() || (t && r.t != t)) value = false;
            if (open()) {
                results.push_back({r, b.GetInsertBlock()});
                b.CreateBr(done);
            }
        }
        b.SetInsertPoint(done);
        if (results.empty()) {  // every clause left (return/break): nothing reaches the end
            if (has_else) b.CreateUnreachable();
            return {nullptr, t_void()};
        }
        if (!value) return {nullptr, t_void()};
        llvm::PHINode *phi = b.CreatePHI(ll(t), results.size());
        for (auto &r : results) phi->addIncoming(r.first.v, r.second);
        return {phi, t};
    }

    // (slice-of a) — an array as a (slice T); (slice-of a from [to]) — part of an
    // array, slice or str (a str's part is a str)
    Val slice_of(Form *f) {
        if (f->size() < 2 || f->size() > 4) fatal(f->loc, "(slice-of a [from [to]])");
        Val data, len;
        Type *st;  // the result type
        Val base = is_place(f->at(1)) ? place(f->at(1)) : spill(expr(f->at(1), nullptr));
        if (base.t->kind == Type::Array) {
            st = t_slice(base.t->elem);
            data = {base.v, t_ptr(base.t->elem)};
            len = {b.getInt64(base.t->n), t_int(64, false)};
        } else if (base.t == t_str() || (base.t->kind == Type::Struct && base.t->st->slice_of)) {
            st = base.t;
            data = {load(field_addr(base, mk_sym(f->loc, "data"))), st->st->fields[0].type};
            len = {load(field_addr(base, mk_sym(f->loc, "len"))), t_int(64, false)};
        } else fatal(f->at(1)->loc, "slice-of needs an array, a slice or a str, got %s", type_name(base.t).c_str());
        Type *u64 = t_int(64, false);
        llvm::Value *from = f->size() >= 3 ? coerce(expr(f->at(2), u64), u64, f->at(2)->loc, "from").v : b.getInt64(0);
        llvm::Value *to = f->size() == 4 ? coerce(expr(f->at(3), u64), u64, f->at(3)->loc, "to").v : len.v;
        Type *elem = data.t->elem;
        llvm::Value *p = b.CreateGEP(ll(elem), data.v, from);
        llvm::Value *v = llvm::UndefValue::get(ll(st));
        v = b.CreateInsertValue(v, p, 0);
        v = b.CreateInsertValue(v, b.CreateSub(to, from), 1);
        return {v, st};
    }

    Val if_(Form *f, Type *want) {
        if (f->size() != 3 && f->size() != 4) fatal(f->loc, "(if cond then [else])");
        llvm::Value *c0 = to_bool(expr(f->at(1), t_bool()), f->at(1)->loc);
        BasicBlock *th = BasicBlock::Create(ctx, "then", fn), *el = BasicBlock::Create(ctx, "else", fn);
        BasicBlock *done = BasicBlock::Create(ctx, "endif", fn);
        b.CreateCondBr(c0, th, el);
        bool two = f->size() == 4;
        // An untyped branch takes the other branch's type, so that one is
        // compiled first (the branches are separate blocks; order is free).
        bool else_first = two && !want && untyped(f->at(2)) && !untyped(f->at(3));
        Type *t = want;
        if (two && !t && untyped(f->at(2)) && untyped(f->at(3))) t = default_type(mk_list(f->loc, {mk_sym(f->loc, "+"), f->at(2), f->at(3)}));
        Val a, x{nullptr, t_void()};
        BasicBlock *th_end, *el_end;
        bool th_open, el_open;
        auto then_branch = [&](Type *tt) {
            b.SetInsertPoint(th);
            a = block(f->at(2), tt);
            th_end = b.GetInsertBlock();
            th_open = open();
        };
        auto else_branch = [&](Type *tt) {
            b.SetInsertPoint(el);
            if (two) x = block(f->at(3), tt);
            el_end = b.GetInsertBlock();
            el_open = open();
        };
        if (else_first) {
            else_branch(nullptr);
            then_branch(x.t);
        } else {
            then_branch(t);
            else_branch(t ? t : a.t);
        }
        bool value = two && a.t != t_void() && x.t != t_void();
        if (value && a.t != x.t) type_error(f->at(3)->loc, "if branches", a.t, x.t);
        if (th_open) { b.SetInsertPoint(th_end); b.CreateBr(done); }
        if (el_open) { b.SetInsertPoint(el_end); b.CreateBr(done); }
        b.SetInsertPoint(done);
        if (!th_open && !el_open) { b.CreateUnreachable(); return {nullptr, t_void()}; }
        if (!value) return {nullptr, t_void()};
        llvm::PHINode *phi = b.CreatePHI(ll(a.t), 2);
        if (th_open) phi->addIncoming(a.v, th_end);
        if (el_open) phi->addIncoming(x.v, el_end);
        return {phi, a.t};
    }

    Val when(Form *f, bool unless) {
        if (f->size() < 2) fatal(f->loc, "(%s cond body...)", unless ? "unless" : "when");
        llvm::Value *c0 = to_bool(expr(f->at(1), t_bool()), f->at(1)->loc);
        BasicBlock *th = BasicBlock::Create(ctx, "when", fn), *done = BasicBlock::Create(ctx, "endwhen", fn);
        if (unless) b.CreateCondBr(c0, done, th);
        else b.CreateCondBr(c0, th, done);
        b.SetInsertPoint(th);
        body(f, 2, nullptr);
        if (open()) b.CreateBr(done);
        b.SetInsertPoint(done);
        return {nullptr, t_void()};
    }

    Val while_(Form *f) {
        if (f->size() < 2) fatal(f->loc, "(while cond body...)");
        BasicBlock *cond = BasicBlock::Create(ctx, "while", fn), *bodyb = BasicBlock::Create(ctx, "do", fn);
        BasicBlock *done = BasicBlock::Create(ctx, "endwhile", fn);
        b.CreateBr(cond);
        b.SetInsertPoint(cond);
        b.CreateCondBr(to_bool(expr(f->at(1), t_bool()), f->at(1)->loc), bodyb, done);
        b.SetInsertPoint(bodyb);
        loops.push_back({cond, done, scopes.size()});
        body(f, 2, nullptr);
        loops.pop_back();
        if (open()) b.CreateBr(cond);
        b.SetInsertPoint(done);
        return {nullptr, t_void()};
    }

    // (for (i [T] start end [step]) body...): i from start while i < end
    Val for_(Form *f) {
        if (f->size() < 2 || !f->at(1)->is_list() || f->at(1)->size() < 3 || f->at(1)->size() > 5 || f->at(1)->at(0)->kind != Form::Sym)
            fatal(f->loc, "(for (i [T] start end [step]) body...)");
        Form *spec = f->at(1);
        Type *given = nullptr;
        if (spec->size() >= 4 && is_type_form(spec->at(1))) {  // drop the type: (i T a b) → (i a b)
            given = parse_type(spec->at(1));
            std::vector<Form *> items = spec->items;
            items.erase(items.begin() + 1);
            spec = mk_list(spec->loc, items);
        }
        if (spec->size() > 4) fatal(f->loc, "(for (i [T] start end [step]) body...)");
        // the counter's type: given, or the first typed bound's (untyped bounds have no side effects)
        Val start, end;
        Type *t;
        if (given) t = given;
        else if (!untyped(spec->at(1))) t = (start = expr(spec->at(1), nullptr)).t;
        else if (!untyped(spec->at(2))) t = (end = expr(spec->at(2), nullptr)).t;
        else t = default_type(mk_list(spec->loc, {mk_sym(spec->loc, "+"), spec->at(1), spec->at(2)}));
        if (!t->is_int()) fatal(spec->loc, "for needs an integer counter, got %s", type_name(t).c_str());
        if (!start.v) start = expr(spec->at(1), t);
        if (!end.v) end = expr(spec->at(2), t);
        if (start.t != t || end.t != t) type_error(spec->loc, "for bounds", t, start.t != t ? start.t : end.t);
        scopes.emplace_back();
        llvm::AllocaInst *iv = alloca_(t, spec->at(0)->text);
        store({iv, t}, start.v);
        scopes.back()[key(spec->at(0))] = {iv, t};
        scopes.back().declared.push_back(spec->at(0));
        if (dsp) declare_var(iv, spec->at(0)->text, t, spec->at(0)->loc);
        BasicBlock *cond = BasicBlock::Create(ctx, "for", fn), *bodyb = BasicBlock::Create(ctx, "body", fn);
        BasicBlock *step = BasicBlock::Create(ctx, "step", fn), *done = BasicBlock::Create(ctx, "endfor", fn);
        b.CreateBr(cond);
        b.SetInsertPoint(cond);
        llvm::Value *i = load({iv, t});
        b.CreateCondBr(t->sign ? b.CreateICmpSLT(i, end.v) : b.CreateICmpULT(i, end.v), bodyb, done);
        b.SetInsertPoint(bodyb);
        loops.push_back({step, done, scopes.size()});
        body(f, 2, nullptr);
        loops.pop_back();
        if (open()) b.CreateBr(step);
        b.SetInsertPoint(step);
        Val by = spec->size() == 4 ? expr(spec->at(3), t) : Val{llvm::ConstantInt::get(ll(t), 1), t};
        if (by.t != t) type_error(spec->at(3)->loc, "for step", t, by.t);
        store({iv, t}, b.CreateAdd(load({iv, t}), by.v));
        b.CreateBr(cond);
        b.SetInsertPoint(done);
        end_scope();
        return {nullptr, t_void()};
    }

    Val jump(Form *f, bool is_break) {
        if (loops.empty()) fatal(f->loc, "(%s) outside a loop", is_break ? "break" : "continue");
        if (in_defer) fatal(f->loc, "(%s) inside a defer", is_break ? "break" : "continue");
        run_defers(loops.back().depth);
        b.CreateBr(is_break ? loops.back().brk : loops.back().cont);
        return {nullptr, t_void()};
    }

    Val return_(Form *f) {
        if (in_defer) fatal(f->loc, "return inside a defer");
        if (f->size() == 1) {
            if (fn_ret != t_void()) fatal(f->loc, "return needs a %s value", type_name(fn_ret).c_str());
            run_defers(0);
            b.CreateRetVoid();
        } else {
            Val v = value_for(f->at(1), fn_ret, "return value");  // computed before the defers run
            run_defers(0);
            emit_ret(v);
        }
        return {nullptr, t_void()};
    }

    Val cast(Val v, Type *to, Loc at) {
        Type *from = v.t;
        if (from == to) return v;
        llvm::Type *lt = ll(to);
        if (from->is_int() && to->is_int()) {
            llvm::Value *r = b.CreateIntCast(v.v, lt, from->sign);
            if (c.checks && fn && (to->bits < from->bits || from->sign != to->sign)) {  // could lose the value
                llvm::Value *ok = b.CreateICmpEQ(b.CreateIntCast(r, ll(from), to->sign), v.v);
                if (from->sign && !to->sign) ok = b.CreateAnd(ok, b.CreateICmpSGE(v.v, llvm::ConstantInt::get(ll(from), 0)));
                if (!from->sign && to->sign) ok = b.CreateAnd(ok, b.CreateICmpSGE(r, llvm::ConstantInt::get(lt, 0)));
                check(ok, at, std::string(from->sign ? "%ld" : "%lu") + " does not fit in " + type_name(to),
                      {b.CreateIntCast(v.v, b.getInt64Ty(), from->sign)});
            }
            return {r, to};
        }
        if (from->kind == Type::Bool && to->is_int()) return {b.CreateZExt(v.v, lt), to};
        if (from->is_int() && to->kind == Type::Bool) return {b.CreateICmpNE(v.v, llvm::ConstantInt::get(ll(from), 0)), to};
        if (from->is_int() && to->is_float()) return {from->sign ? b.CreateSIToFP(v.v, lt) : b.CreateUIToFP(v.v, lt), to};
        if (from->is_float() && to->is_int()) {
            if (c.checks && fn) {  // in range (and not NaN): the ordered compares fail on NaN
                llvm::Type *ft = ll(from);
                double hi = std::ldexp(1.0, to->sign ? to->bits - 1 : to->bits);
                llvm::Value *ok = b.CreateAnd(to->sign ? b.CreateFCmpOGE(v.v, llvm::ConstantFP::get(ft, -hi))
                                                       : b.CreateFCmpOGT(v.v, llvm::ConstantFP::get(ft, -1.0)),
                                              b.CreateFCmpOLT(v.v, llvm::ConstantFP::get(ft, hi)));
                check(ok, at, "%g does not fit in " + type_name(to), {b.CreateFPExt(v.v, b.getDoubleTy())});
            }
            return {to->sign ? b.CreateFPToSI(v.v, lt) : b.CreateFPToUI(v.v, lt), to};
        }
        if (from->is_float() && to->is_float()) return {b.CreateFPCast(v.v, lt), to};
        if (from->is_ptr() && to->is_ptr()) return {v.v, to};
        if (from->is_ptr() && to->is_int()) return {b.CreatePtrToInt(v.v, lt), to};
        if (from->is_int() && to->is_ptr()) return {b.CreateIntToPtr(v.v, lt), to};
        fatal(at, "cannot cast %s to %s", type_name(from).c_str(), type_name(to).c_str());
    }

    // same bits, other type (f32 <-> u32 ...)
    Val bitcast(Val v, Type *to, Loc at) {
        if (!v.t->is_scalar() || !to->is_scalar() || size_of(v.t) != size_of(to))
            fatal(at, "cannot bitcast %s to %s", type_name(v.t).c_str(), type_name(to).c_str());
        if (v.t->is_ptr() || to->is_ptr()) return cast(v, to, at);
        return {b.CreateBitCast(v.v, ll(to)), to};
    }

    // (make T :field e ...) for structs, (make (array T N) e ...) for arrays; the rest is zero
    Val make(Form *f) {
        if (f->size() < 2) fatal(f->loc, "(make T ...)");
        Type *t = parse_type(f->at(1));
        llvm::AllocaInst *a = temp(t);  // (a make inside a field's value gets another)
        store({a, t}, llvm::Constant::getNullValue(ll(t)));
        if (t->kind == Type::Struct) {
            ensure_layout(t->st, f->loc);
            if ((f->size() - 2) % 2) fatal(f->loc, "(make %s :field value ...)", type_name(t).c_str());
            for (size_t i = 2; i < f->size(); i += 2) {
                Form *k = f->at(i);
                if (k->kind != Form::Sym || k->text.size() < 2 || k->text[0] != ':') fatal(k->loc, "expected :field");
                Val fa = field_addr({a, t}, mk_sym(k->loc, k->text.substr(1)));
                if (f->at(i + 1)->is_sym("undefined")) continue;  // (it stays zero: make zeroes first)
                store(fa, value_for(f->at(i + 1), fa.t, "field").v);
            }
        } else if (t->kind == Type::Array) {
            if (f->size() - 2 > t->n) fatal(f->loc, "too many elements for %s", type_name(t).c_str());
            for (size_t i = 2; i < f->size(); i++) {
                llvm::Value *p = b.CreateInBoundsGEP(ll(t), a, {b.getInt64(0), b.getInt64(i - 2)});
                store({p, t->elem}, value_for(f->at(i), t->elem, "element").v);
            }
        } else fatal(f->loc, "make needs a struct or array type");
        Val r{load({a, t}), t};
        done(t, a);
        return r;
    }

    Val call(Form *f) {
        Form *head = f->at(0);
        Val callee;
        Type *ft = nullptr;
        Global *g = nullptr;
        if (head->kind == Form::Sym && !lookup_local(key(head)).v) {
            g = lookup_global(head);
            if (!g) {  // a generic function: for the types of the arguments
                std::string gk = generic_key(head, generic_fns);
                if (!gk.empty()) g = infer_fn(generic_fns[gk], f);
            }
            if (!g) fatal(head->loc, "unknown function '%s'", head->text.c_str());
        } else if (head->is_list() && head->size() >= 2 && head->at(0)->kind == Form::Sym && !lookup_local(key(head->at(0))).v) {
            std::string gk = generic_key(head->at(0), generic_fns);  // ((name T ...) args...)
            if (!gk.empty()) {
                Generic &gen = generic_fns[gk];
                g = instantiate_fn(gen, type_args(gen, head), head->loc);
            }
        }
        if (g && g->kind == Global::Func) { callee = {g->value, g->type}; ft = g->type; }
        if (!ft) {
            callee = expr(head, nullptr);
            if (!callee.t->is_ptr() || callee.t->elem->kind != Type::Fn) fatal(head->loc, "%s is not a function", show(head).c_str());
            ft = callee.t->elem;
        }
        size_t n = f->size() - 1;
        if (n < ft->params.size() || (n > ft->params.size() && !ft->variadic))
            fatal(f->loc, "%s takes %zu argument%s, got %zu", show(head).c_str(), ft->params.size(), ft->params.size() == 1 ? "" : "s", n);
        std::vector<Val> fixed, var;
        for (size_t i = 0; i < n; i++) {
            Form *a = f->at(i + 1);
            if (i < ft->params.size()) {
                fixed.push_back(value_for(a, ft->params[i], "argument"));
                continue;
            }
            // variadic: C's default promotions; arrays decay to a pointer to their first element
            if (is_cstring_lit(a)) {
                var.push_back(expr(a, t_ptr(t_int(8, true))));
                continue;
            }
            if (is_place(a)) {
                Val p = place(a);
                if (p.t->kind == Type::Array) {
                    var.push_back({p.v, t_ptr(p.t->elem)});
                    continue;
                }
            }
            Val v = expr(a, nullptr);
            if (v.t->is_float() && v.t->bits < 64) v = cast(v, t_float(64), a->loc);
            else if (v.t->is_int() && v.t->bits < 32) v = cast(v, t_int(32, v.t->sign), a->loc);
            else if (v.t->kind == Type::Bool) v = cast(v, t_int(32, true), a->loc);
            else if (v.t == t_str()) fatal(a->loc, "cannot pass a str to a variadic function: for printf, \"%%.*s\" with (cast i32 s.len) and s.data, or a c\"...\" literal");
            else if (v.t->kind == Type::Struct || v.t->kind == Type::Array) fatal(a->loc, "cannot pass %s to a variadic function", type_name(v.t).c_str());
            var.push_back(v);
        }
        return emit_call(callee.v, ft, fixed, var);
    }

    // a call with its arguments compiled, lowered per the calling convention
    Val emit_call(llvm::Value *callee, Type *ft, const std::vector<Val> &fixed, const std::vector<Val> &var) {
        FnAbi &abi = fn_abi(ft);
        std::vector<llvm::Value *> args;
        llvm::Value *sret = nullptr;
        if (abi.ret.kind == ArgAbi::Memory) args.push_back(sret = temp(ft->elem));
        for (size_t i = 0; i < fixed.size(); i++) {
            const ArgAbi &pa = abi.params[i];
            if (pa.kind == ArgAbi::Direct) args.push_back(fixed[i].v);
            else if (pa.kind == ArgAbi::Coerce) coerce_out(pa, fixed[i], args);
            else if (pa.kind == ArgAbi::Memory) args.push_back(spill(fixed[i]).v);  // byval: the callee gets its own copy
        }
        for (const Val &v : var) args.push_back(v.v);
        llvm::CallInst *ci = b.CreateCall(abi.ty, callee, args);
        ci->setAttributes(abi.attrs);
        Type *rt = ft->elem;
        switch (abi.ret.kind) {
        case ArgAbi::Direct: return {ci, rt};
        case ArgAbi::Ignore: return {llvm::Constant::getNullValue(ll(rt)), rt};
        case ArgAbi::Memory: {
            Val r{load({sret, rt}), rt};
            done(rt, llvm::cast<llvm::AllocaInst>(sret));
            return r;
        }
        case ArgAbi::Coerce: {
            llvm::AllocaInst *m = temp(rt);
            std::vector<llvm::Value *> parts;
            if (abi.ret.parts.size() == 1) parts.push_back(ci);
            else
                for (unsigned i = 0; i < abi.ret.parts.size(); i++) parts.push_back(b.CreateExtractValue(ci, i));
            coerce_in(abi.ret, parts, m, rt);
            Val r{load({m, rt}), rt};
            done(rt, m);
            return r;
        }
        }
        return {};
    }

    /* ------------------------------------------------------ top level --- */

    // top-level macro calls expanded; (do ...) at the top level is spliced
    void expand_top(Form *f, std::vector<Form *> &out) {
        f = expand(f);
        if (f->is_call("do")) {
            for (size_t i = 1; i < f->size(); i++) expand_top(f->at(i), out);
        } else out.push_back(f);
    }

    ModInfo *root = nullptr;  // the program's first file

    // the top-level forms of every module, each with its module set as cur
    template <class F> void each(const std::vector<std::pair<Form *, ModInfo *>> &fs, F fn) {
        for (auto &it : fs) {
            cur = it.second;
            fn(it.first);
        }
        cur = nullptr;
    }

    void top(const std::vector<Form *> &files) {
        if (files.empty()) return;
        std::vector<Form *> fs = files;
        root = load_module(files[0]->loc.file, files[0]->loc, &fs);
        // macros and compile-time helpers first, so everything can use them
        std::vector<std::pair<Form *, ModInfo *>> all;
        each(items, [&](Form *f) {
            if (f->is_call("defmacro") || f->is_call("defmeta")) {
                if (f->size() < 2 || f->at(1)->kind != Form::Sym) fatal(f->loc, "(%s name (params...) body...)", f->head());
                std::string k = def_key(f->at(1));
                meta.define(f, k);
                note_private(f->at(1), k);
            } else all.push_back({f, cur});
        });
        // pass 0: the C modules' headers
        for (auto &n : c_mods) {
            c.c_prefix = n + "::";
            c_include(c, mods[n].headers, files[0]->loc);
        }
        c.c_prefix.clear();
        // pass 1: struct names, so structs can refer to each other
        std::vector<std::pair<Form *, StructInfo *>> structs;
        std::vector<ModInfo *> struct_mods;
        each(all, [&](Form *f) {
            if (f->is_call("enum")) return define_enum(f);
            if (!f->is_call("struct") && !f->is_call("union")) return;
            if (f->size() >= 2 && f->at(1)->is_list()) return register_generic(f, false);
            if (f->size() < 2 || f->at(1)->kind != Form::Sym) fatal(f->loc, "(struct name (field T) ...)");
            auto *st = new StructInfo;
            st->name = f->at(1)->text;
            st->is_union = f->is_call("union");
            std::string k = def_key(f->at(1));
            if (c.types_.count(k)) fatal(f->loc, "type '%s' is already defined", st->name.c_str());
            c.types_[k] = t_struct(st);
            note_private(f->at(1), k);
            structs.push_back({f, st});
            struct_mods.push_back(cur);
        });
        add_fields(structs, struct_mods);
        for (auto &s : structs) lay_out(s.second, s.first->loc);
        // top-level macros run once the structs are known (they may ask about them);
        // structs they produce are added the same way
        std::vector<std::pair<Form *, ModInfo *>> expanded;
        each(all, [&](Form *f) {
            if (f->is_call("struct") || f->is_call("union") || f->is_call("enum")) {
                expanded.push_back({f, cur});
                return;
            }
            std::vector<Form *> out;
            expand_top(f, out);
            for (Form *x : out) expanded.push_back({x, cur});
        });
        all = expanded;
        std::vector<std::pair<Form *, StructInfo *>> more;
        std::vector<ModInfo *> more_mods;
        each(all, [&](Form *f) {
            if ((f->is_call("struct") || f->is_call("union")) && f->size() >= 2 && f->at(1)->is_list() && f->at(1)->size() &&
                !generic_structs.count(def_key(f->at(1)->at(0))))
                return register_generic(f, false);  // (made by a macro)
            if ((!f->is_call("struct") && !f->is_call("union")) || f->size() < 2 || f->at(1)->kind != Form::Sym) return;
            std::string k = def_key(f->at(1));
            if (c.types_.count(k) && c.types_[k]->kind == Type::Struct && c.types_[k]->st->complete) return;
            auto *st = new StructInfo;
            st->name = f->at(1)->text;
            st->is_union = f->is_call("union");
            c.types_[k] = t_struct(st);
            note_private(f->at(1), k);
            more.push_back({f, st});
            more_mods.push_back(cur);
        });
        add_fields(more, more_mods);
        for (auto &s : more) lay_out(s.second, s.first->loc);
        for (size_t i = 0; i < pending_layout.size(); i++) lay_out(pending_layout[i].first, pending_layout[i].second);
        pending_layout.clear();
        structs_ready = true;
        // defop: a function named after the operator and its types
        std::vector<std::pair<std::string, std::string>> ops;  // operator, qualified function name
        for (auto &item : all) {
            Form *&f = item.first;
            cur = item.second;
            if (!f->is_call("defop")) continue;
            if (f->size() < 5 || f->at(1)->kind != Form::Sym || !f->at(2)->is_list()) fatal(f->loc, "(defop op ((a T) ...) R body...)");
            std::string name = f->at(1)->text + "(";
            for (size_t i = 0; i < f->at(2)->size(); i++) {
                Form *p = f->at(2)->at(i);
                if (!p->is_list() || p->size() != 2) fatal(p->loc, "parameter must be (name T)");
                name += (i ? "," : "") + type_name(parse_type(p->at(1)));
            }
            name += ")";
            std::vector<Form *> items = f->items;
            items[0] = mk_sym(f->loc, "defn");
            items[1] = mk_sym(f->at(1)->loc, name);
            std::string op = f->at(1)->text;
            f = mk_list(f->loc, items);
            ops.push_back({op, def_key(f->at(1))});
        }
        cur = nullptr;
        // pass 2: constants, globals, function signatures
        each(all, [&](Form *f) {
            const char *h = f->head();
            if (!strcmp(h, "const")) {
                if (f->size() != 3 || f->at(1)->kind != Form::Sym) fatal(f->loc, "(const NAME expr)");
                define(f->at(1), Global{Global::Const, nullptr, nullptr, f->at(2)});
            } else if (!strcmp(h, "def")) {
                def_global(f);
            } else if (!strcmp(h, "defn")) {
                if (f->size() >= 2 && f->at(1)->is_list()) register_generic(f, true);
                else declare_fn(f);
            } else if (!strcmp(h, "extern")) {
                if (f->size() != 4 || f->at(1)->kind != Form::Sym || !f->at(2)->is_list()) fatal(f->loc, "(extern name (T ...) R)");
                Type *ft = parse_type(mk_list(f->loc, {mk_sym(f->loc, "fn"), f->at(2), f->at(3)}));
                c.c_prefix = cur->name + "::";  // module::name, the C symbol name
                c.add_extern_fn(f->at(1)->text, ft);
                c.c_prefix.clear();
            } else if (!strcmp(h, "export")) {
                exports.push_back({f, cur});
            } else if (strcmp(h, "struct") && strcmp(h, "union") && strcmp(h, "enum")) {
                fatal(f->loc, "unknown top-level form %s", show(f).c_str());
            }
        });
        for (auto &o : ops) overloads[o.first].push_back(&c.globals_[o.second]);
        // (export name ...): these functions and globals keep their own name, for C
        for (auto &e : exports) {
            cur = e.second;
            for (size_t i = 1; i < e.first->size(); i++) {
                Form *n = e.first->at(i);
                if (n->kind != Form::Sym) fatal(n->loc, "(export name ...)");
                auto it = c.globals_.find(def_key(n));
                if (it == c.globals_.end() || it->second.kind == Global::Const)
                    fatal(n->loc, "export: no function or global '%s' in this module", n->text.c_str());
                auto *gv = llvm::cast<llvm::GlobalValue>(it->second.value);
                if (gv->getName() != n->text && mod.getNamedValue(n->text))
                    fatal(n->loc, "export: '%s' is already a symbol of the program (a C function?)", n->text.c_str());
                gv->setName(n->text);
                gv->setLinkage(llvm::GlobalValue::ExternalLinkage);
            }
        }
        cur = nullptr;
        // pass 3: function bodies. An error in one is reported and the next is
        // compiled anyway (up to 20 errors); the program is not produced.
        auto guarded = [&](auto body, const PendingFn *inst) {
            jmp_buf here;
            if (setjmp(here) == 0) {
                error_trap = &here;
                body();
            } else {
                if (inst) fprintf(stderr, "%s:%d:%d: note: in %s, used here\n", inst->at.file, inst->at.line, inst->at.col, inst->shown.c_str());
                abandon_fn();
                if (error_count >= 20) {
                    fprintf(stderr, "lisc: too many errors, stopping\n");
                    exit(1);
                }
            }
            error_trap = nullptr;
        };
        each(all, [&](Form *f) {
            if (!f->is_call("defn") || (f->size() >= 2 && f->at(1)->is_list())) return;
            guarded([&] { define_fn(f); }, nullptr);
        });
        // the generic functions' instances; compiling one can ask for more
        for (size_t i = 0; i < pending_fns.size(); i++) {
            PendingFn p = pending_fns[i];
            cur = p.mod;
            tbind = p.bind;
            guarded([&] { define_fn(p.def, p.key, p.shown); }, &p);
            tbind.clear();
            cur = nullptr;
        }
        if (error_count) {
            fprintf(stderr, "lisc: %d error%s\n", error_count, error_count == 1 ? "" : "s");
            exit(1);
        }
    }

    std::vector<std::pair<Form *, ModInfo *>> exports;

    // (enum name T a b (c 5) d ...): a distinct integer type; values count up from
    // 0, or from the last one given
    void define_enum(Form *f) {
        if (f->size() < 3 || f->at(1)->kind != Form::Sym) fatal(f->loc, "(enum name T member ...)");
        Type *base = parse_type(f->at(2));
        if (!base->is_int() || base->en) fatal(f->at(2)->loc, "an enum's type is an integer type, got %s", type_name(base).c_str());
        auto *en = new EnumInfo{f->at(1)->text, {}};
        uint64_t next = 0;
        for (size_t i = 3; i < f->size(); i++) {
            Form *m = f->at(i);
            Form *name = m->is_list() && m->size() == 2 ? m->at(0) : m;
            if (name->kind != Form::Sym) fatal(m->loc, "an enum member is a name or (name value)");
            if (m->is_list()) {
                if (m->at(1)->kind != Form::Int) fatal(m->at(1)->loc, "an enum value is an integer");
                next = m->at(1)->ival;
            }
            for (auto &v : en->values)
                if (v.first == name->text) fatal(name->loc, "%s is twice in enum %s", name->text.c_str(), en->name.c_str());
            en->values.push_back({name->text, next++});
        }
        std::string k = def_key(f->at(1));
        if (c.types_.count(k)) fatal(f->loc, "type '%s' is already defined", en->name.c_str());
        c.types_[k] = t_enum(en, base);
        note_private(f->at(1), k);
    }

    void add_fields(std::vector<std::pair<Form *, StructInfo *>> &structs, std::vector<ModInfo *> &ms) {
        for (size_t si = 0; si < structs.size(); si++) {
            auto &s = structs[si];
            cur = ms[si];
            size_t first = 2;
            // options before the fields: :packed, :align N
            while (first < s.first->size() && s.first->at(first)->kind == Form::Sym) {
                Form *o = s.first->at(first);
                if (o->is_sym(":packed")) s.second->packed = true;
                else if (o->is_sym(":align") && first + 1 < s.first->size() && s.first->at(first + 1)->kind == Form::Int) {
                    uint64_t a = s.first->at(++first)->ival;
                    if (!a || (a & (a - 1))) fatal(s.first->at(first)->loc, ":align needs a power of two");
                    s.second->min_align = a;
                } else fatal(o->loc, "struct options are :packed and :align N");
                first++;
            }
            for (size_t i = first; i < s.first->size(); i++) {

                Form *fd = s.first->at(i);
                bool bits = fd->is_list() && fd->size() == 4 && fd->at(2)->is_sym(":bits") && fd->at(3)->kind == Form::Int;
                if (!fd->is_list() || (fd->size() != 2 && !bits) || fd->at(0)->kind != Form::Sym)
                    fatal(fd->loc, "field must be (name T) or (name T :bits N)");
                Field field{fd->at(0)->text, parse_type(fd->at(1)), 0};
                if (bits) {
                    field.bit_width = (unsigned)fd->at(3)->ival;
                    if (!field.type->is_int() && field.type->kind != Type::Bool) fatal(fd->loc, "a bit field must be an integer or bool");
                    if (field.bit_width == 0 || field.bit_width > 8 * size_of(field.type)) fatal(fd->at(3)->loc, "bit width must be 1..%lu", 8 * size_of(field.type));
                }
                s.second->fields.push_back(field);
            }
        }
        cur = nullptr;
    }

    void lay_out(StructInfo *st, Loc at) {
        if (st->complete) return;
        if (laying_out[st]) fatal(at, "struct %s contains itself", st->name.c_str());
        laying_out[st] = true;
        for (auto &fd : st->fields) {
            Type *t = fd.type;
            while (t->kind == Type::Array) t = t->elem;
            if (t->kind == Type::Struct) lay_out(t->st, at);
        }
        layout_struct(st);
    }

    void define(Form *name, Global g) {
        std::string k = def_key(name);
        auto it = c.globals_.find(k);
        if (it != c.globals_.end() && !it->second.imported) fatal(name->loc, "'%s' is already defined", name->text.c_str());
        c.globals_[k] = g;
        note_private(name, k);
    }

    void def_global(Form *f) {
        if (f->size() < 3 || f->size() > 4 || f->at(1)->kind != Form::Sym) fatal(f->loc, "(def name T [init])");
        Type *t = parse_type(f->at(2));
        llvm::Constant *init = llvm::Constant::getNullValue(ll(t));  // zero unless given (or undefined)
        if (f->size() == 4) init = f->at(3)->is_sym("undefined") ? llvm::UndefValue::get(ll(t)) : constant(f->at(3), t);
        auto *gv = new llvm::GlobalVariable(mod, ll(t), false, llvm::GlobalValue::InternalLinkage, init, def_key(f->at(1)));
        if (dib && real_file(f->loc.file) && ditype(t))
            gv->addDebugInfo(dib->createGlobalVariableExpression(cu, f->at(1)->text, gv->getName(), difile(f->loc.file),
                                                                 f->loc.line, ditype(t), true));
        gv->setAlignment(llvm::Align(align_of(t)));
        define(f->at(1), Global{Global::Var, t, gv});
    }

    // a compile-time constant of type t: an expression that folds, or a
    // (make T ...) of constants (arrays and structs, nested)
    llvm::Constant *constant(Form *f, Type *t) {
        f = expand(f);
        if (f->is_call("make") && f->size() >= 2) {
            Type *mt = parse_type(f->at(1));
            if (mt != t) type_error(f->loc, "initializer", t, mt);
            if (t->kind == Type::Array) {
                if (f->size() - 2 > t->n) fatal(f->loc, "too many elements for %s", type_name(t).c_str());
                std::vector<llvm::Constant *> es;
                for (uint64_t i = 0; i < t->n; i++)
                    es.push_back(i + 2 < f->size() ? constant(f->at(i + 2), t->elem) : llvm::Constant::getNullValue(ll(t->elem)));
                return llvm::ConstantArray::get(llvm::cast<llvm::ArrayType>(ll(t)), es);
            }
            if (t->kind == Type::Struct) {
                // the parts of ll_struct, in the same order: fields by offset, padding as zero bytes
                std::map<std::string, Form *> given;
                for (size_t i = 2; i + 1 < f->size(); i += 2) given[f->at(i)->text.substr(1)] = f->at(i + 1);
                if ((f->size() - 2) % 2) fatal(f->loc, "(make %s :field value ...)", type_name(t).c_str());
                std::vector<const Field *> order;
                for (auto &fd : t->st->fields) {
                    if (fd.bit_width && given.count(fd.name)) fatal(f->loc, "bit fields in constant initializers are not supported");
                    if (!fd.bit_width) order.push_back(&fd);
                }
                for (auto &g : given)
                    if (!t->st->find(g.first)) fatal(g.second->loc, "%s has no field '%s'", type_name(t).c_str(), g.first.c_str());
                std::stable_sort(order.begin(), order.end(), [](const Field *a, const Field *x) { return a->offset < x->offset; });
                auto *st = llvm::cast<llvm::StructType>(ll(t));
                std::vector<llvm::Constant *> parts;
                uint64_t cur = 0;
                for (const Field *fd : order) {
                    if (fd->offset < cur) continue;
                    if (fd->offset > cur) parts.push_back(llvm::Constant::getNullValue(st->getElementType(parts.size())));
                    auto it = given.find(fd->name);
                    parts.push_back(it != given.end() ? constant(it->second, fd->type) : llvm::Constant::getNullValue(ll(fd->type)));
                    cur = fd->offset + size_of(fd->type);
                }
                if (parts.size() < st->getNumElements()) parts.push_back(llvm::Constant::getNullValue(st->getElementType(parts.size())));
                return llvm::ConstantStruct::get(st, parts);
            }
        }
        Val v = expr(f, t);
        auto *k = llvm::dyn_cast<llvm::Constant>(coerce(v, t, f->loc, "initializer").v);
        if (!k) fatal(f->loc, "a global's initializer must be a constant");
        return k;
    }

    // a defn's first body form: after the name, parameters, result type and options
    static size_t body_start(Form *f) {
        size_t i = 4;
        while (i < f->size() && f->at(i)->kind == Form::Sym && f->at(i)->text.size() > 1 && f->at(i)->text[0] == ':') i++;
        return i;
    }

    // (ikey: a generic's instance, by its table key)
    void declare_fn(Form *f, const std::string &ikey = "") {
        if (f->size() < 4 || (ikey.empty() && f->at(1)->kind != Form::Sym) || !f->at(2)->is_list()) fatal(f->loc, "(defn name ((a T) ...) R body...)");
        std::vector<Type *> ps;
        for (Form *p : f->at(2)->items) {
            if (!p->is_list() || p->size() != 2 || p->at(0)->kind != Form::Sym) fatal(p->loc, "parameter must be (name T)");
            ps.push_back(parse_type(p->at(1)));
        }
        Type *ft = t_fn(parse_type(f->at(3)), ps, false);
        // the program's main (in the first module) is the C entry point; everything else is internal
        bool entry = ikey.empty() && f->at(1)->text == "main" && cur == root;
        std::string name = entry ? "main" : !ikey.empty() ? ikey : def_key(f->at(1));
        auto link = entry ? llvm::GlobalValue::ExternalLinkage : llvm::GlobalValue::InternalLinkage;
        llvm::Function *fn0 = llvm::Function::Create(fn_abi(ft).ty, link, name, mod);
        fn0->setAttributes(fn_abi(ft).attrs);
        // options after the result type: :noinline :inline :cold
        for (size_t i = 4; i < body_start(f); i++) {
            Form *o = f->at(i);
            if (o->is_sym(":noinline")) fn0->addFnAttr(llvm::Attribute::NoInline);
            else if (o->is_sym(":inline")) fn0->addFnAttr(llvm::Attribute::AlwaysInline);
            else if (o->is_sym(":cold")) fn0->addFnAttr(llvm::Attribute::Cold);
            else if (o->is_sym(":optnone")) {  // compiled as at -O0 (and never inlined)
                fn0->addFnAttr(llvm::Attribute::OptimizeNone);
                fn0->addFnAttr(llvm::Attribute::NoInline);
            } else fatal(o->loc, "function options are :noinline :inline :cold :optnone");
        }
        if (!ikey.empty()) c.globals_[ikey] = Global{Global::Func, ft, fn0, nullptr, f};
        else define(f->at(1), Global{Global::Func, ft, fn0, nullptr, f});
    }

    // after an error in a function body: drop what was built, reset the state
    void abandon_fn() {
        if (fn) fn->deleteBody();
        tbind.clear();
        temp_pool.clear();
        scopes.clear();
        loops.clear();
        type_memo.clear();
        in_defer = false;
        dsp = nullptr;
        b.ClearInsertionPoint();
        b.SetCurrentDebugLocation(llvm::DebugLoc());
        meta.reset();
    }

    void define_fn(Form *f, const std::string &ikey = "", const std::string &shown = "") {
        Global &g = c.globals_[ikey.empty() ? def_key(f->at(1)) : ikey];
        const std::string fname = shown.empty() ? f->at(1)->text : shown;
        fn = llvm::cast<llvm::Function>(g.value);
        fn_ret = g.type->elem;
        cur_ft = g.type;
        type_memo.clear();
        temp_pool.clear();
        FnAbi &abi = fn_abi(g.type);
        b.SetInsertPoint(BasicBlock::Create(ctx, "entry", fn));
        dsp = nullptr;
        if (dib) {
            Loc at = user_loc(f->loc);
            if (!real_file(at.file)) at = {cu->getFile()->getFilename().data(), 1, 0};
            llvm::DIFile *file = real_file(at.file) ? difile(at.file) : cu->getFile();
            auto *sty = llvm::cast<llvm::DISubroutineType>(ditype(g.type));
            auto flags = llvm::DISubprogram::SPFlagDefinition;
            if (fn->hasInternalLinkage()) flags |= llvm::DISubprogram::SPFlagLocalToUnit;
            dsp = dib->createFunction(file, fname, fn->getName(), file, at.line, sty, at.line,
                                      llvm::DINode::FlagPrototyped, flags);
            fn->setSubprogram(dsp);
            set_loc(f->loc);
        }
        scopes.assign(1, {});
        auto arg = fn->arg_begin();
        cur_sret = abi.ret.kind == ArgAbi::Memory ? &*arg++ : nullptr;
        for (size_t i = 0; i < g.type->params.size(); i++) {
            const std::string &name = f->at(2)->at(i)->at(0)->text;
            Type *t = g.type->params[i];
            const ArgAbi &pa = abi.params[i];
            if (pa.kind == ArgAbi::Memory) {  // byval: the copy is ours to use as the variable
                arg->setName(name);
                scopes.back()[key(f->at(2)->at(i)->at(0))] = {&*arg, t};
                if (dsp) declare_var(&*arg, name, t, f->at(2)->at(i)->loc, i + 1);
                arg++;
                continue;
            }
            llvm::AllocaInst *a = alloca_(t, name);
            if (pa.kind == ArgAbi::Direct) {
                arg->setName(name);
                store({a, t}, &*arg++);
            } else if (pa.kind == ArgAbi::Coerce) {
                std::vector<llvm::Value *> parts;
                for (size_t k = 0; k < pa.parts.size(); k++) parts.push_back(&*arg++);
                coerce_in(pa, parts, a, t);
            } else {
                store({a, t}, llvm::Constant::getNullValue(ll(t)));
            }
            scopes.back()[key(f->at(2)->at(i)->at(0))] = {a, t};
            if (dsp) declare_var(a, name, t, f->at(2)->at(i)->loc, i + 1);
        }
        Val v = body(f, body_start(f), fn_ret == t_void() ? nullptr : fn_ret);
        if (open()) {
            if (fn_ret == t_void()) b.CreateRetVoid();
            else {
                if (v.t == t_void()) fatal(f->loc, "%s must end with a %s value or return", fname.c_str(), type_name(fn_ret).c_str());
                emit_ret(coerce(v, fn_ret, f->at(f->size() - 1)->loc, "result"));
            }
        }
        // blocks left empty by control flow that never falls through
        for (BasicBlock &bb : *fn)
            if (!bb.getTerminator()) {
                b.SetInsertPoint(&bb);
                b.CreateUnreachable();
            }
        scopes.clear();
        dsp = nullptr;
        b.SetCurrentDebugLocation(llvm::DebugLoc());
    }
};

Compiler::Compiler() : ctx_(new llvm::LLVMContext), mod_(new llvm::Module("lisc", *ctx_)), impl_(new Impl(*this)) {}

// str: a string with a size, not NUL-terminated: (data (ptr u8)) (len u64)
Type *t_str() {
    static Type *t = [] {
        auto *st = new StructInfo;
        st->name = "str";
        st->fields = {{"data", t_ptr(t_int(8, false)), 0}, {"len", t_int(64, false), 8}};
        st->size = 16;
        st->align = 8;
        st->complete = true;
        return t_struct(st);
    }();
    return t;
}
Compiler::~Compiler() = default;

void Compiler::compile(const std::vector<Form *> &forms) {
    if (debug && !forms.empty()) impl_->debug_init(forms[0]->loc.file);
    impl_->top(forms);
    if (impl_->dib) impl_->dib->finalize();
    std::string err;
    llvm::raw_string_ostream os(err);
    if (llvm::verifyModule(*mod_, &os)) {
        fprintf(stderr, "internal error: invalid LLVM IR:\n%s\n", err.c_str());
        exit(2);
    }
}

void Compiler::add_type(const std::string &name, Type *t) { types_[c_prefix + name] = t; }

void Compiler::define_const(const std::string &name, const std::string &value) {
    char *end;
    strtod(value.c_str(), &end);
    bool number = !value.empty() && !*end;
    Form *lit = new Form{number ? (value.find_first_of(".eE") == std::string::npos ? Form::Int : Form::Float) : Form::Str, {"<-D>", 0, 0}};
    lit->text = value;
    lit->cstr = true;  // command-line text is for C, like a C macro
    if (lit->kind == Form::Int) lit->ival = strtoull(value.c_str(), nullptr, 0);
    globals_[name] = Global{Global::Const, nullptr, nullptr, lit};
}

// The C importer's declarations go into the module being imported: the key
// is c_prefix + name (libc::printf), the symbol keeps the C name (printf).
void Compiler::put_c(const std::string &name, Global g) {
    g.imported = true;
    globals_[c_prefix + name] = g;
}

void Compiler::add_extern_fn(const std::string &name, Type *fn) {
    if (has_global(name)) return;
    llvm::Function *f = mod_->getFunction(name);
    if (!f) {
        f = llvm::Function::Create(llvm::cast<llvm::FunctionType>(impl_->ll(fn)), llvm::GlobalValue::ExternalLinkage, name, *mod_);
        f->setAttributes(impl_->fn_abi(fn).attrs);
    }
    put_c(name, Global{Global::Func, fn, f});
}

void Compiler::add_extern_var(const std::string &name, Type *t) {
    if (has_global(name)) return;
    llvm::GlobalVariable *gv = mod_->getGlobalVariable(name);
    if (!gv) gv = new llvm::GlobalVariable(*mod_, impl_->ll(t), false, llvm::GlobalValue::ExternalLinkage, nullptr, name);
    put_c(name, Global{Global::Var, t, gv});
}

// C's #define and enum constants are untyped, as in C: they take the type
// they meet, like a literal written in their place
void Compiler::add_const_int(const std::string &name, Type *t, uint64_t v) {
    if (has_global(name)) return;
    Form *lit = new Form{Form::Int, {"<c>", 0, 0}};
    lit->ival = v;
    lit->text = t->sign ? std::to_string((int64_t)v) : std::to_string(v);
    put_c(name, Global{Global::Const, nullptr, nullptr, lit});
}

void Compiler::add_const_float(const std::string &name, Type *t, double v) {
    if (has_global(name)) return;
    Form *lit = new Form{Form::Float, {"<c>", 0, 0}};
    char buf[64];
    snprintf(buf, sizeof buf, "%.17g", v);
    lit->text = buf;
    if (lit->text.find_first_of(".eEn") == std::string::npos) lit->text += ".0";
    (void)t;
    put_c(name, Global{Global::Const, nullptr, nullptr, lit});
}

void Compiler::add_const_str(const std::string &name, const std::string &s) {
    if (has_global(name)) return;
    llvm::Constant *init = llvm::ConstantDataArray::getString(*ctx_, s);
    auto *gv = new llvm::GlobalVariable(*mod_, init->getType(), true, llvm::GlobalValue::PrivateLinkage, init, name);
    put_c(name, Global{Global::Const, t_ptr(t_int(8, true)), gv});
}
