// form.h — source forms (s-expressions) and error reporting.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Loc {
    const char *file = "?";
    int line = 0, col = 0;
};

// Print "file:line:col: error: ..." and exit. The compiler stops at the first error.
[[noreturn]] void fatal(Loc at, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

struct Form {
    enum Kind { Int, Float, Sym, Str, List, Fn } kind;  // Fn: a compile-time closure (macro.cpp)
    Loc loc;
    uint64_t ival = 0;          // Int (two's complement of the written value)
    std::string text;           // Sym name, Str contents, Int/Float as written
    std::vector<Form *> items;  // List
    void *fn = nullptr;         // Fn
    // Sym: hygiene mark. A symbol written in a macro's template gets the
    // expansion's mark; it is bound and looked up as a different name from
    // the same symbol without the mark (see key() in compiler.cpp).
    unsigned mark = 0;
    // Sym: the module whose source the symbol was read from (or whose macro
    // template it was copied from); null for symbols the compiler made up
    const char *mod = nullptr;

    bool is_sym(const char *s) const { return kind == Sym && text == s; }
    bool is_list() const { return kind == List; }
    size_t size() const { return items.size(); }
    Form *at(size_t i) const { return items[i]; }
    // (head ...) with a symbol head named `s`
    bool is_call(const char *s) const { return kind == List && !items.empty() && items[0]->is_sym(s); }
    const char *head() const { return kind == List && !items.empty() && items[0]->kind == Sym ? items[0]->text.c_str() : ""; }
};

// Forms live for the whole compilation and are never freed.
Form *mk_sym(Loc at, const std::string &name);
Form *mk_list(Loc at, std::vector<Form *> items);

// all top-level forms of a file
std::vector<Form *> read_file(const char *path);
// readable text of a form, for error messages
std::string show(const Form *f);
