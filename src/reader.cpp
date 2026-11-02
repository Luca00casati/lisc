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
        return c < 0 || strchr(" \t\r\n()\";'`,", c) != nullptr;
    }

    Form *read() {
        skip_space();
        Loc at = loc();
        int c = peek();
        if (c < 0) fatal(at, "unexpected end of file");
        if (c == ')') fatal(at, "unexpected ')'");
        if (c == '(') {
            next();
            std::vector<Form *> items;
            for (;;) {
                skip_space();
                if (peek() < 0) fatal(at, "unclosed '('");
                if (peek() == ')') { next(); break; }
                items.push_back(read());
            }
            return mk_list(at, std::move(items));
        }
        if (c == '\'' || c == '`' || c == ',') {
            next();
            const char *name = c == '\'' ? "quote" : c == '`' ? "quasiquote" : "unquote";
            if (c == ',' && peek() == '@') { next(); name = "unquote-splicing"; }
            return mk_list(at, {mk_sym(at, name), read()});
        }
        if (c == '"') return read_string(at);
        if (c == '#' && pos + 1 < src.size() && src[pos + 1] == '\\') return read_char(at);
        if (c == 'c' && pos + 1 < src.size() && src[pos + 1] == '"') {  // c"...": a C string
            next();
            Form *f = read_string(at);
            f->cstr = true;
            return f;
        }
        std::string tok;
        while (!delimiter(peek())) tok += (char)next();
        return atom(at, tok);
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
    return forms;
}

std::string show(const Form *f) {
    switch (f->kind) {
    case Form::Int: case Form::Float: case Form::Sym: return f->text;
    case Form::Str: return (f->cstr ? "c\"" : "\"") + f->text + "\"";
    case Form::Fn: return "<fn>";
    case Form::List: {
        std::string s = "(";
        for (size_t i = 0; i < f->items.size(); i++) s += (i ? " " : "") + show(f->items[i]);
        return s + ")";
    }
    }
    return "?";
}
