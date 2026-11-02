// form.h — source forms (s-expressions) and error reporting.
#pragma once

#include <csetjmp>
#include <cstdint>
#include <string>
#include <vector>

struct Expansion;

struct Loc {
    const char *file = "?";
    int line = 0, col = 0;
    // code a macro produced: the expansion it came from (for error messages)
    const Expansion *via = nullptr;
};

// a macro call that produced code: the macro, and where it was called
struct Expansion {
    std::string macro;
    Loc at;
};

// Print "file:line:col: error: ...", plus "in expansion of" lines for code a
// macro produced, and exit — or, while error_trap is set (compiling a
// function body), count it and jump there, so the next function is compiled.
[[noreturn]] void fatal(Loc at, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

extern jmp_buf *error_trap;
extern int error_count;

// "file:line:col: warning: ..." (unless warnings are off: lisc -w)
void warn(Loc at, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
extern bool warnings;

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
    // Str: c"..." — a C string (NUL-terminated, a pointer) rather than a str
    bool cstr = false;
    // List: written [ ... ] — a binding: (let [x 1] [y 2] body), (defn f [a T] [b U] R body),
    // (for [i 0 n] ...), (struct s [x f32] ...)
    bool bracket = false;
    // Sym: the module whose source the symbol was read from (or whose macro
    // template it was copied from); null for symbols the compiler made up
    const char *mod = nullptr;
    // Sym: the type itself, for a symbol type-of made: it means this type
    // wherever it lands, whatever its name would resolve to there
    const struct Type *type = nullptr;
    // creation order: forms newer than an expansion's start were made by it
    uint64_t id = next_id++;
    static inline uint64_t next_id = 0;

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

// f with the bracket forms [a T] [b U] ... starting at item i gathered into one
// list ((a T) (b U)), as if written the old way; a lone [] is an empty list
Form *unbracket(Form *f, size_t i);

// all top-level forms of a file
std::vector<Form *> read_file(const char *path);
// readable text of a form, for error messages
std::string show(const Form *f);
