// fmt.cpp — lisc fmt: the canonical layout of lisc source.
//
// Line breaks stay where they are written; what is normalised is everything
// else: indentation, the spaces between tokens, trailing whitespace and runs
// of blank lines. So formatting a file never reflows code, and its diff is
// only the whitespace that was off.
//
// Indentation, for a line inside an open bracket:
//   - a body form (defn let when for match struct ...): its body 2 past the
//     paren; a let's / defn's further [bindings] under the first one
//   - another call: under its first argument, if that is on the paren's line,
//     else 1 past the paren (so do lists of data)
//   - inside [ ]: 1 past the bracket; inside { }: as a call
// Tokens written touching (a[i], 'x, ,@xs, c"...") stay touching; other gaps
// become one space, none just inside a bracket. A comment after code keeps
// its column when the code leaves room for it.

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {

struct Open {
    char ch;
    int col, line;         // where the bracket is, in the output
    std::string head;      // the first element, when it is a name
    int items = 0;
    int first_arg_col = -1;  // the second element's column, if on the bracket's line
    int first_binding_col = -1;  // the first [ ] element's column (after the head)
};

const std::set<std::string> body_forms = {
    "defn", "defn-", "defop", "defmacro", "defmacro-", "defmeta", "defmeta-", "fn", "let", "when", "unless", "while", "for",
    "match", "case", "defer", "if-defined", "def", "def-", "const", "const-", "struct", "struct-", "union", "union-", "variant", "enum", "private", "%alias", "lambda"};
const std::set<std::string> binding_forms = {"defn", "defn-", "defop", "fn", "let", "%alias"};

bool delim(int c) { return c == 0 || strchr(" \t\r\n()[]{}\";'`,", c) != nullptr; }

struct Formatter {
    const std::string &src;
    std::string out;
    std::vector<Open> stack;
    size_t pos = 0;
    int line = 0;         // output line number
    int col = 0;          // output column on the current line
    bool in_string = false;
    bool prefix = false;  // a ' ` , ,@ waits for its form

    explicit Formatter(const std::string &s) : src(s) {}

    // the column a line starting with `c` gets
    int indent(char c) {
        if (stack.empty()) return 0;
        Open &p = stack.back();
        if (c == ')' || c == ']' || c == '}') return p.col;  // (a closer alone on its line)
        if (p.ch == '[') return p.col + 1;
        if (p.ch == '(' && body_forms.count(p.head)) {
            if (c == '[' && binding_forms.count(p.head) && p.first_binding_col >= 0) return p.first_binding_col;
            return p.col + 2;
        }
        if (p.items == 0 || (p.head.empty() && p.ch == '(')) return p.col + 1;
        if (p.first_arg_col >= 0) return p.first_arg_col;
        return p.head == "do" ? p.col + 2 : p.col + 1;  // (do on its own line, then its body
    }

    void put(const std::string &s) {
        out += s;
        col += (int)s.size();
    }

    // a new element of the innermost list starts at the current column
    void element(const std::string &atom, bool opener) {
        if (prefix) { prefix = false; return; }  // ('x is one element)
        if (stack.empty()) return;
        Open &p = stack.back();
        int i = p.items++;
        if (i == 0 && !opener) p.head = atom;
        if (i == 1 && line == p.line) p.first_arg_col = col;
        if (i >= 1 && opener && src[pos] == '[' && p.first_binding_col < 0) p.first_binding_col = col;
    }

    // one source line (without its newline); `at_start` = we are not inside a string
    void line_of(size_t start, size_t end) {
        size_t i = start;
        bool first = true;
        char last = 0;  // the previous token's kind: '(' opener, ')' closer, 'p' prefix, 'a' other
        if (in_string) {  // the rest of a string: as it is (then the line goes on)
            string_body(i, end);
            if (in_string) { pos = end; return; }
            first = false;
            last = 'a';
        } else {
            while (i < end && (src[i] == ' ' || src[i] == '\t' || src[i] == '\r')) i++;
            if (i == end) return;  // (blank: the caller counts these)
            int ind = src[i] == ';' ? indent(next_code(end)) : indent(src[i]);  // (a comment line: as the code after it)
            put(std::string(ind, ' '));
        }
        while (i < end) {
            size_t ws = i;
            while (i < end && (src[i] == ' ' || src[i] == '\t' || src[i] == '\r')) i++;
            if (i == end) break;
            bool gap = i > ws;
            char c = src[i];
            if (c == ';') {  // a comment: the rest of the line
                if (!first) {
                    int want = (int)(i - start);  // its column in the source
                    put(want > col ? std::string(want - col, ' ') : std::string(" "));
                }
                std::string cm = src.substr(i, end - i);
                while (!cm.empty() && (cm.back() == ' ' || cm.back() == '\t' || cm.back() == '\r')) cm.pop_back();
                put(cm);
                break;
            }
            bool closer = c == ')' || c == ']' || c == '}';
            if (!first && gap && last != '(' && last != 'p' && !closer) put(" ");
            first = false;
            pos = i;
            if (c == '(' || c == '[' || c == '{') {
                bool index = c == '[' && !gap && (last == 'a' || last == ')');  // a[i]: an index, not an element
                if (!index) element("", true);
                stack.push_back({c, col, line, "", 0, -1, -1});
                put(std::string(1, c));
                i++;
                last = '(';
            } else if (closer) {
                if (!stack.empty()) stack.pop_back();
                put(std::string(1, c));
                i++;
                last = ')';
            } else if (c == '\'' || c == '`' || c == ',') {
                element("", false);
                prefix = true;
                size_t j = i + 1 + (c == ',' && i + 1 < end && src[i + 1] == '@');
                put(src.substr(i, j - i));
                i = j;
                last = 'p';
            } else if (c == '"' || (c == 'c' && i + 1 < end && src[i + 1] == '"')) {
                element("", false);
                size_t j = i + (c == 'c') + 1;
                put(src.substr(i, j - i));
                in_string = true;
                string_body(j, end);
                i = j;
                last = 'a';
            } else {
                size_t j = i;
                if (c == '#' && i + 1 < end && src[i + 1] == '\\') j += 3;  // #\( #\; : the character is taken whatever it is
                while (j < end && !delim(src[j])) j++;
                std::string tok = src.substr(i, j - i);
                // .x right after an index is part of it (a[i].x), not an element
                bool attached = !gap && (last == ')' || last == 'a');
                if (!attached) element(tok, false);
                else prefix = false;
                put(tok);
                i = j;
                last = 'a';
            }
        }
        pos = end;
    }

    // the first character of the next line that has code (not a comment), from `from`
    char next_code(size_t from) {
        size_t i = from;
        while (i < src.size()) {
            while (i < src.size() && (src[i] == ' ' || src[i] == '\t' || src[i] == '\r' || src[i] == '\n')) i++;
            if (i < src.size() && src[i] == ';') {
                while (i < src.size() && src[i] != '\n') i++;
                continue;
            }
            break;
        }
        return i < src.size() ? src[i] : 'x';
    }

    // inside a string from i: copy up to its end (or the line's), leaving i after it
    void string_body(size_t &i, size_t end) {
        size_t j = i;
        while (j < end) {
            if (src[j] == '\\' && j + 1 < end) { j += 2; continue; }
            if (src[j] == '"') { j++; in_string = false; break; }
            j++;
        }
        put(src.substr(i, j - i));
        i = j;
    }

    std::string run() {
        int blank = 0;
        size_t i = 0;
        bool any = false;
        while (i < src.size()) {
            size_t e = src.find('\n', i);
            if (e == std::string::npos) e = src.size();
            bool was_string = in_string;
            size_t k = i;
            while (k < e && (src[k] == ' ' || src[k] == '\t' || src[k] == '\r')) k++;
            if (k == e && !was_string) {
                blank++;
            } else {
                if (any && !was_string) out += blank ? "\n\n" : "\n";
                else if (was_string) out += "\n";
                blank = 0;
                any = true;
                col = 0;
                line_of(i, e);
                line++;
            }
            i = e + 1;
        }
        if (any) out += "\n";
        return out;
    }
};

}  // namespace

std::string format_source(const std::string &src) { return Formatter(src).run(); }

// lisc fmt [--check] file ...: rewrite each file in the canonical layout
// (--check: change nothing, list the files that aren't, exit 1 if any)
int fmt_main(int argc, char **argv) {
    bool check = false;
    int bad = 0, files = 0;
    for (int a = 2; a < argc; a++) {
        if (!strcmp(argv[a], "--check")) { check = true; continue; }
        files++;
        FILE *f = fopen(argv[a], "rb");
        if (!f) { fprintf(stderr, "lisc fmt: cannot open %s\n", argv[a]); bad++; continue; }
        std::string src;
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) src.append(buf, n);
        fclose(f);
        std::string out = format_source(src);
        if (out == src) continue;
        bad++;
        if (check) { printf("%s\n", argv[a]); continue; }
        f = fopen(argv[a], "wb");
        if (!f || fwrite(out.data(), 1, out.size(), f) != out.size()) { fprintf(stderr, "lisc fmt: cannot write %s\n", argv[a]); return 1; }
        fclose(f);
    }
    if (!files) { fprintf(stderr, "usage: lisc fmt [--check] file.lsp ...\n"); return 1; }
    return check && bad ? 1 : 0;
}
