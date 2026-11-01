// reader.cpp — text → forms.
//
//   ; comment            (a b c)             "string\n"
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
    exit(1);
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
        std::string tok;
        while (!delimiter(peek())) tok += (char)next();
        return atom(at, tok);
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
                case 'x': {
                    char h[3] = {(char)next(), (char)next(), 0};
                    c = (int)strtol(h, nullptr, 16);
                    break;
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

std::vector<Form *> read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) fatal({path, 0, 0}, "cannot open: %s", strerror(errno));
    Reader r{path};
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) r.src.append(buf, n);
    fclose(f);
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
    case Form::Str: return "\"" + f->text + "\"";
    case Form::Fn: return "<fn>";
    case Form::List: {
        std::string s = "(";
        for (size_t i = 0; i < f->items.size(); i++) s += (i ? " " : "") + show(f->items[i]);
        return s + ")";
    }
    }
    return "?";
}
