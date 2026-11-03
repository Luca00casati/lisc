// reader.cpp — text → forms.
//
//   ; comment            (a b c)             "string\n"  (UTF-8; \u{1F600}, \xHH raw byte)
//   c"C string"  (NUL-terminated, for C)     #\a #\é #\newline  (a character: its code point)
//   123  -5  0x1f        1.5  2e-3  -0.5     symbols: anything else
//   'x  `x  ,x  ,@x  →  (quote x) (quasiquote x) (unquote x) (unquote-splicing x)
//   a.b.c  →  (. a b c)  field access (a symbol with inner dots)

#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "form.h"

void fatal(Loc at, const char *fmt, ...) {
    fprintf(stderr, "%s:%d:%d: error: ", at.file, at.line, at.col);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    for (const Expansion *e = at.via; e; e = e->at.via)
        fprintf(stderr, "%s:%d:%d: note: in expansion of macro %s\n", e->at.file, e->at.line, e->at.col, e->macro.c_str());
    error_count++;
    if (error_trap) longjmp(*error_trap, 1);
    exit(1);
}

jmp_buf *error_trap = nullptr;
int error_count = 0;
bool warnings = true;

void warn(Loc at, const char *fmt, ...) {
    if (!warnings) return;
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    // once per place (a generic's body is compiled for each of its instances)
    static std::set<std::string> said;
    if (!said.insert(std::string(at.file) + ":" + std::to_string(at.line) + ":" + std::to_string(at.col) + msg).second) return;
    fprintf(stderr, "%s:%d:%d: warning: %s\n", at.file, at.line, at.col, msg);
    for (const Expansion *e = at.via; e; e = e->at.via)
        fprintf(stderr, "%s:%d:%d: note: in expansion of macro %s\n", e->at.file, e->at.line, e->at.col, e->macro.c_str());
}

Form *mk_sym(Loc at, const std::string &name) {
    Form *f = new Form{Form::Sym, at};
    f->text = name;
    return f;
}

Form *mk_list(Loc at, std::vector<Form *> items) {
    Form *f = new Form{Form::List, at};
    f->items = std::move(items);
    return f;
}

namespace {

struct Reader {
    const char *file;
    std::string src;
    size_t pos = 0;
    int line = 1, col = 1;

    Loc loc() const { return {file, line, col}; }
    int peek() const { return pos < src.size() ? (unsigned char)src[pos] : -1; }
    int next() {
        int c = peek();
        if (c < 0) return c;
        pos++;
        if (c == '\n') { line++; col = 1; } else col++;
        return c;
    }

    void skip_space() {
        for (;;) {
            int c = peek();
            if (c == ';') while (peek() >= 0 && peek() != '\n') next();
            else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') next();
            else return;
        }
    }

    static bool delimiter(int c) {
        return c < 0 || strchr(" \t\r\n()[]{}\";'`,", c) != nullptr;
    }

    // the forms up to `close`, after `open` at `at`
    std::vector<Form *> read_until(Loc at, char open, char close) {
        std::vector<Form *> items;
        for (;;) {
            skip_space();
            int c = peek();
            if (c < 0) fatal(at, "unclosed '%c'", open);
            if (c == close) { next(); return items; }
            if (c == ')' || c == ']' || c == '}') fatal(loc(), "'%c' at %d:%d closed by '%c'", open, at.line, at.col, c);
            items.push_back(read());
        }
    }

    // a form, then any a[i] indexing and .field access written right after it:
    // v.data[i] → (aref (. v data) i), a[i][j].x → (. (aref (aref a i) j) x)
    Form *read() {
        Form *f = primary();
        bool indexed = false;
        for (;;) {
            Loc at = loc();
            if (peek() == '[' && (f->kind == Form::Sym || (f->is_list() && !f->bracket))) {
                next();
                std::vector<Form *> ix = read_until(at, '[', ']');
                if (ix.size() != 1) fatal(at, "an index takes one form: a[i]");
                f = mk_list(at, {mk_sym(at, "aref"), f, ix[0]});
                indexed = true;
            } else if (peek() == '.' && indexed) {
                next();
                std::vector<Form *> items{mk_sym(at, "."), f};
                for (;;) {
                    std::string name;
                    while (!delimiter(peek()) && peek() != '.') name += (char)next();
                    if (name.empty()) fatal(at, "a field name must follow '.'");
                    items.push_back(mk_sym(at, name));
                    if (peek() != '.') break;
                    next();
                }
                f = mk_list(at, std::move(items));
            } else return f;
        }
    }

    Form *primary() {
        skip_space();
        Loc at = loc();
        int c = peek();
        if (c < 0) fatal(at, "unexpected end of file");
        if (c == ')' || c == ']' || c == '}') fatal(at, "unexpected '%c'", c);
        Form *f;
        if (c == '(') {
            next();
            f = mk_list(at, read_until(at, '(', ')'));
        } else if (c == '[') {  // [x T init]: a binding
            next();
            f = mk_list(at, read_until(at, '[', ']'));
            f->bracket = true;
        } else if (c == '{') {  // {T :field v ...} = (make T :field v ...)
            next();
            std::vector<Form *> items = read_until(at, '{', '}');
            if (items.empty()) fatal(at, "{} needs a type: {point :x 1.0 :y 2.0}");
            items.insert(items.begin(), mk_sym(at, "make"));
            f = mk_list(at, std::move(items));
        } else if (c == '\'' || c == '`' || c == ',') {
            next();
            const char *name = c == '\'' ? "quote" : c == '`' ? "quasiquote" : "unquote";
            if (c == ',' && peek() == '@') { next(); name = "unquote-splicing"; }
            Form *inner = primary();  // ,v[i] is (aref ,v i)
            f = mk_list(at, {mk_sym(at, name), inner});
        } else if (c == '"') f = read_string(at);
        else if (c == '#' && pos + 1 < src.size() && src[pos + 1] == '\\') f = read_char(at);
        else if (c == 'c' && pos + 1 < src.size() && src[pos + 1] == '"') {  // c"...": a C string
            next();
            f = read_string(at);
            f->cstr = true;
        } else {
            std::string tok;
            while (!delimiter(peek())) tok += (char)next();
            f = atom(at, tok);
        }
        return f;
    }

    // #\a #\é #\space #\newline #\tab #\nul #\return #\escape: the code point,
    // an untyped integer (like a number written there)
    Form *read_char(Loc at) {
        next();
        next();
        std::string tok;
        int c0 = next();  // the first character is taken whatever it is: #\( #\;
        if (c0 < 0) fatal(at, "#\\ needs a character");
        tok += (char)c0;
        if ((c0 & 0xC0) == 0xC0)  // the rest of a UTF-8 sequence
            while ((peek() & 0xC0) == 0x80) tok += (char)next();
        while (!delimiter(peek())) tok += (char)next();
        static const std::pair<const char *, uint32_t> names[] = {
            {"space", 32}, {"newline", 10}, {"tab", 9}, {"nul", 0}, {"return", 13}, {"escape", 27}, {"backspace", 8}};
        uint32_t cp = 0;
        bool found = false;
        for (auto &n : names)
            if (tok == n.first) { cp = n.second; found = true; }
        if (!found) {
            unsigned char b0 = tok[0];
            size_t n = b0 < 0x80 ? 1 : (b0 >> 5) == 6 ? 2 : (b0 >> 4) == 14 ? 3 : 4;
            if (tok.size() != n) fatal(at, "unknown character #\\%s", tok.c_str());
            cp = n == 1 ? b0 : n == 2 ? b0 & 0x1F : n == 3 ? b0 & 0x0F : b0 & 0x07;
            for (size_t k = 1; k < n; k++) cp = cp << 6 | ((unsigned char)tok[k] & 0x3F);
        }
        Form *f = new Form{Form::Int, at};
        f->ival = cp;
        f->text = std::to_string(cp);
        return f;
    }

    Form *read_string(Loc at) {
        next();
        Form *f = new Form{Form::Str, at};
        for (;;) {
            int c = next();
            if (c < 0) fatal(at, "unterminated string");
            if (c == '"') break;
            if (c == '\\') {
                c = next();
                switch (c) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '0': c = 0; break;
                case '\\': case '"': break;
                case 'x': {  // a raw byte
                    char h[3] = {(char)next(), (char)next(), 0};
                    c = (int)strtol(h, nullptr, 16);
                    break;
                }
                case 'u': {  // \u{1F600}: a code point, written as UTF-8
                    Loc u = loc();
                    if (next() != '{') fatal(u, "\\u needs braces: \\u{1F600}");
                    std::string hex;
                    while (peek() >= 0 && peek() != '}') hex += (char)next();
                    next();
                    char *end;
                    unsigned long cp = strtoul(hex.c_str(), &end, 16);
                    if (hex.empty() || *end || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) fatal(u, "bad code point \\u{%s}", hex.c_str());
                    if (cp < 0x80) f->text += (char)cp;
                    else if (cp < 0x800) { f->text += (char)(0xC0 | cp >> 6); f->text += (char)(0x80 | (cp & 0x3F)); }
                    else if (cp < 0x10000) { f->text += (char)(0xE0 | cp >> 12); f->text += (char)(0x80 | (cp >> 6 & 0x3F)); f->text += (char)(0x80 | (cp & 0x3F)); }
                    else { f->text += (char)(0xF0 | cp >> 18); f->text += (char)(0x80 | (cp >> 12 & 0x3F)); f->text += (char)(0x80 | (cp >> 6 & 0x3F)); f->text += (char)(0x80 | (cp & 0x3F)); }
                    continue;
                }
                default: fatal(loc(), "unknown escape '\\%c'", c);
                }
            }
            f->text += (char)c;
        }
        return f;
    }

    Form *atom(Loc at, const std::string &tok) {
        const char *s = tok.c_str();
        const char *d = s + (*s == '-' || *s == '+');
        if (isdigit((unsigned char)*d) || (*d == '.' && isdigit((unsigned char)d[1]))) {
            char *end;
            bool hex = d[0] == '0' && (d[1] == 'x' || d[1] == 'X');
            bool flt = !hex && strpbrk(d, ".eE");
            if (!flt) {
                errno = 0;
                uint64_t v = strtoull(d, &end, 0);
                if (*end || errno) fatal(at, "bad integer '%s'", s);
                Form *f = new Form{Form::Int, at};
                f->ival = *s == '-' ? (uint64_t)0 - v : v;
                f->text = tok;
                return f;
            }
            strtod(s, &end);
            if (*end) fatal(at, "bad number '%s'", s);
            Form *f = new Form{Form::Float, at};
            f->text = tok;
            return f;
        }
        // a.b.c → (. a b c)
        size_t dot = tok.find('.');
        if (dot != std::string::npos && dot > 0 && tok.back() != '.' && tok.find("..") == std::string::npos) {
            std::vector<Form *> items{mk_sym(at, ".")};
            size_t start = 0;
            for (;;) {
                size_t e = tok.find('.', start);
                items.push_back(mk_sym(at, tok.substr(start, e - start)));
                if (e == std::string::npos) break;
                start = e + 1;
            }
            return mk_list(at, std::move(items));
        }
        return mk_sym(at, tok);
    }
};

}  // namespace

// lisc source is UTF-8 (so string literals are UTF-8 too)
static void check_utf8(const char *path, const std::string &s) {
    int line = 1, col = 1;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = n && i + n <= s.size();
        for (size_t k = 1; ok && k < n; k++) ok = ((unsigned char)s[i + k] >> 6) == 2;
        if (ok && n > 1) {  // no overlong forms, surrogates or values past U+10FFFF
            uint32_t cp = n == 2 ? c & 0x1F : n == 3 ? c & 0x0F : c & 0x07;
            for (size_t k = 1; k < n; k++) cp = cp << 6 | ((unsigned char)s[i + k] & 0x3F);
            ok = cp >= (n == 2 ? 0x80u : n == 3 ? 0x800u : 0x10000u) && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
        }
        if (!ok) fatal({path, line, col}, "the source is not valid UTF-8 here");
        if (c == '\n') { line++; col = 1; } else col++;
        i += n;
    }
}

namespace {

// a list written with ( ) where a binding goes ([x T]); ,x and ,@x in a
// template stand for whatever they are given, so they pass
bool old_binding(const Form *f) {
    return f->is_list() && !f->bracket && !f->is_call("unquote") && !f->is_call("unquote-splicing");
}

[[noreturn]] void old_spelling(const Form *f, const char *want) {
    fatal(f->loc, "bindings are written in [ ]: %s (tools/brackets.py rewrites old code)", want);
}

// bindings have one spelling, [ ]: (let ((x 1)) ...), (defn f ((a T)) R ...),
// (for (i 0 n) ...) and (struct s (x T)) are errors in written source
void check_bindings(const Form *f) {
    if (!f->is_list() || f->is_call("quote")) return;
    if (f->is_call("let") && f->size() >= 2 && old_binding(f->at(1)))
        old_spelling(f->at(1), "(let [x 1] [y T 2] body...)");
    if ((f->is_call("defn") || f->is_call("defop")) && f->size() >= 3 && old_binding(f->at(2)))
        old_spelling(f->at(2), f->at(2)->items.empty() ? "[] for no parameters" : "(defn f [a T] [b U] R body...)");
    if (f->is_call("for") && f->size() >= 2 && old_binding(f->at(1)))
        old_spelling(f->at(1), "(for [i 0 n] body...), (for [x :in xs] body...)");
    if (f->is_call("struct") || f->is_call("union"))
        for (size_t i = 2; i < f->size(); i++)
            if (old_binding(f->at(i))) old_spelling(f->at(i), "(struct name [field T] ...)");
    if (f->is_call("variant"))
        for (size_t i = 2; i < f->size(); i++)
            if (f->at(i)->is_list())
                for (size_t j = 1; j < f->at(i)->size(); j++)
                    if (old_binding(f->at(i)->at(j))) old_spelling(f->at(i)->at(j), "(variant name (case [field T] ...) ...)");
    for (const Form *x : f->items) check_bindings(x);
}

}  // namespace

std::vector<Form *> read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) fatal({path, 0, 0}, "cannot open: %s", strerror(errno));
    Reader r{path};
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) r.src.append(buf, n);
    fclose(f);
    check_utf8(path, r.src);
    std::vector<Form *> forms;
    for (;;) {
        r.skip_space();
        if (r.peek() < 0) break;
        forms.push_back(r.read());
    }
    for (Form *f : forms) check_bindings(f);
    return forms;
}

Form *unbracket(Form *f, size_t i) {
    if (f->size() <= i || !f->at(i)->bracket) return f;
    size_t j = i;
    while (j < f->size() && f->at(j)->bracket) j++;
    std::vector<Form *> bs(f->items.begin() + i, f->items.begin() + j);
    if (bs.size() == 1 && bs[0]->items.empty()) bs.clear();
    for (Form *b : bs)
        if (b->items.empty()) fatal(b->loc, "[] stands alone, for no bindings at all");
    std::vector<Form *> items(f->items.begin(), f->items.begin() + i);
    items.push_back(mk_list(f->at(i)->loc, bs));
    items.insert(items.end(), f->items.begin() + j, f->items.end());
    Form *r = new Form(*f);  // (the same form otherwise: its place, its id)
    r->items = std::move(items);
    return r;
}

std::string show(const Form *f) {
    switch (f->kind) {
    case Form::Int: case Form::Float: case Form::Sym: return f->text;
    case Form::Str: return (f->cstr ? "c\"" : "\"") + f->text + "\"";
    case Form::Fn: return "<fn>";
    case Form::List: {
        std::string s = f->bracket ? "[" : "(";
        for (size_t i = 0; i < f->items.size(); i++) s += (i ? " " : "") + show(f->items[i]);
        return s + (f->bracket ? "]" : ")");
    }
    }
    return "?";
}
