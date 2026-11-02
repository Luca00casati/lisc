// macro.h — the compile-time Lisp that runs macros.
//
// Values are forms (the program's own data): numbers, strings, symbols and
// lists, plus closures. true/false are the symbols true and false; () and
// false are false, everything else is true.
//
//   (defmacro name (a b &rest more) body...)   a macro: body computes a form
//   (defmeta name (a b &rest more) body...)    a compile-time helper function
//
// Inside them:
//   quote ' quasiquote ` unquote , unquote-splicing ,@
//   (if c a [b]) (when c ...) (unless c ...) (cond (c e...) ...) (do ...)
//   (let ((x e) ...) body...) (set x e) (while c body...) (lambda (args) body...)
//   (and ...) (or ...) (not x)
//   lists:   list cons first rest nth length append reverse map mapcat filter range empty?
//   parameters destructure: (defmacro m ((name n) &body body) ...)
//   tests:   symbol? list? int? float? string? eq (structural)  = != < <= > >= (numbers)
//   numbers: + - * / %
//   names:   (sym a b ...) → symbol from pieces   (str a b ...) → string, (cstr ...) → c"..."   gensym   symbol-name
//   types:   (type-of e) — the type e has where the macro is used, e.g. f32 or (ptr v3)
//            (type-kind T) → void bool int float ptr array struct fn
//            (type-elem T) (type-len T) (type-fields T) → ((name T) ...)   (type-size T)
//   (error "msg" ...) stops compilation at the macro call;  (print x ...) debugs to stderr
//
// Hygiene: every symbol written in a quasiquote template gets a mark unique
// to the expansion. Variables the template binds (let, for, parameters) are
// then distinct from the same names at the use site, both ways, and names the
// template uses without binding mean the global definitions, not a local that
// happens to be in scope where the macro is used. Arguments keep their own
// names. To capture a name on purpose, insert an unmarked symbol: ,'it
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "form.h"
#include "type.h"

// what the macro language needs from the compiler
struct MetaHost {
    virtual ~MetaHost() = default;
    virtual Type *type_of(Form *expr) = 0;  // without emitting code
    virtual Type *parse_type(Form *t) = 0;
};

class Meta {
public:
    explicit Meta(MetaHost &host);
    // a defmacro (under its qualified name `key`, see modules in compiler.cpp)
    // or defmeta form (compile-time helpers are global)
    void define(Form *def, const std::string &key);
    bool is_macro(const std::string &key) const { return macros_.count(key) != 0; }
    // expand one macro call (its head names the macro `key`)
    Form *expand(Form *call, const std::string &key);

    struct Env;
    struct Closure;

private:
    Form *eval(Form *f, Env *env);
    Form *apply(Form *fn, const std::vector<Form *> &args, Loc at);
    Form *call_closure(Closure *c, const std::vector<Form *> &args, Loc at);
    Form *quasi(Form *f, Env *env, int depth);
    Form *type_form(Type *t, Loc at);
    void stamp(Form *f, uint64_t start, const Expansion *e);

    MetaHost &host_;
    std::map<std::string, Closure *> macros_, metas_;
    std::map<std::string, std::function<Form *(std::vector<Form *> &, Loc)>> builtins_;
    Loc call_at_;  // the macro call being expanded (for errors)
    int gensym_ = 0;
    unsigned mark_ = 0, marks_ = 0;  // hygiene: the current expansion's mark, the last one handed out
};
