// cimport.cpp — C modules ((import "libc"), (import-c "h" ...)): the
// declarations of real headers, read with libclang, become lisc declarations
// under the module's name:
//
//   functions with external linkage  → extern functions
//   extern variables                 → global variables (stderr, environ, ...)
//   structs and unions               → struct types, with clang's exact layout
//   typedefs                         → type names (uint32_t, size_t, FILE ...)
//   enum constants                   → typed constants
//   object-like #defines             → typed constants, if clang can evaluate
//                                      them (so DRM_IOCTL_MODE_GETCRTC works)
//
// long double is f80 and __int128 i128/u128. Types lisc has no equivalent for
// (vectors, _Complex) are skipped: a declaration that needs one is left out,
// as are functions returning a struct with a long double inside. Bit fields
// are kept.
//
// An import is a list of declarations (Decl), made by libclang or read back
// from the cache: ~/.cache/lisc (or $XDG_CACHE_HOME/lisc) keeps each C
// module's list, keyed by its headers, the compiler flags and the lisc
// binary, and checked against every file clang read (path, size, mtime).
// Evaluating a module's #defines is most of the import's time; with the
// cache, a build does no libclang work at all. --no-cache turns it off.

#include <clang-c/Index.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "compiler.h"

namespace {

std::string str(CXString s) {
    std::string r = clang_getCString(s) ? clang_getCString(s) : "";
    clang_disposeString(s);
    return r;
}

const char *const MACRO_PREFIX = "__lisc_macro_";

// one StructInfo per C struct, however many C modules declare it
std::map<std::string, StructInfo *> &all_records() {
    static std::map<std::string, StructInfo *> m;
    return m;
}

// ... and each record's USR, for writing the cache
std::map<StructInfo *, std::string> &record_usrs() {
    static std::map<StructInfo *, std::string> m;
    return m;
}

// one declaration of a C module, in the order clang met them
struct Decl {
    enum Kind { Fn, Var, TypeName, Int, Float, Str } kind;
    std::string name;
    Type *t = nullptr;
    uint64_t i = 0;
    double f = 0;
    std::string s;
};

struct Importer {
    Loc at;
    std::map<std::string, StructInfo *> &records = all_records();  // by USR, shared by every C module
    std::vector<std::string> macros;
    std::vector<Decl> decls;
    std::set<std::string> globals, types;  // the names taken so far (as Compiler::has_global / has_type)

    void global(Decl::Kind k, const std::string &name, Type *t, uint64_t i = 0, double f = 0, const std::string &s = "") {
        if (!globals.insert(name).second) return;
        decls.push_back({k, name, t, i, f, s});
    }
    void type(const std::string &name, Type *t) {
        if (!types.insert(name).second) return;
        decls.push_back({Decl::TypeName, name, t});
    }

    Type *conv(CXType t) {
        t = clang_getCanonicalType(t);
        switch (t.kind) {
        case CXType_Void: return t_void();
        case CXType_Bool: return t_bool();
        case CXType_Char_S: case CXType_SChar: case CXType_Short: case CXType_Int: case CXType_Long:
        case CXType_LongLong: case CXType_WChar:
            return t_int(8 * clang_Type_getSizeOf(t), true);
        case CXType_Char_U: case CXType_UChar: case CXType_UShort: case CXType_UInt: case CXType_ULong:
        case CXType_ULongLong: case CXType_Char16: case CXType_Char32:
            return t_int(8 * clang_Type_getSizeOf(t), false);
        case CXType_Float: return t_float(32);
        case CXType_Double: return t_float(64);
        case CXType_LongDouble: return clang_Type_getSizeOf(t) == 16 ? t_float(80) : nullptr;
        case CXType_Int128: return t_int(128, true);
        case CXType_UInt128: return t_int(128, false);
        case CXType_Pointer: {
            Type *e = conv(clang_getPointeeType(t));
            return t_ptr(e ? e : t_void());
        }
        case CXType_ConstantArray: {
            Type *e = conv(clang_getArrayElementType(t));
            return e ? t_array(e, clang_getArraySize(t)) : nullptr;
        }
        case CXType_IncompleteArray: {
            Type *e = conv(clang_getArrayElementType(t));
            return e ? t_array(e, 0) : nullptr;
        }
        case CXType_Vector: {  // __attribute__((vector_size(N))), __m128 ...
            Type *e = conv(clang_getElementType(t));
            long long n = clang_getNumElements(t);
            return e && e->is_num() && n > 0 && !(n & (n - 1)) ? t_simd(e, (uint64_t)n) : nullptr;
        }
        case CXType_Enum: return conv(clang_getEnumDeclIntegerType(clang_getTypeDeclaration(t)));
        case CXType_Record: return t_struct(record(clang_getTypeDeclaration(t)));
        case CXType_FunctionProto: case CXType_FunctionNoProto: {
            Type *ret = conv(clang_getResultType(t));
            // a struct with a long double inside comes back in x87 registers: not supported
            if (!ret || (ret->kind == Type::Struct && has_f80(ret))) return nullptr;
            std::vector<Type *> ps;
            int n = clang_getNumArgTypes(t);
            for (int i = 0; i < n; i++) {
                Type *p = conv(clang_getArgType(t, i));
                if (!p || p->kind == Type::Array) return nullptr;
                ps.push_back(p);
            }
            return t_fn(ret, ps, t.kind == CXType_FunctionNoProto || clang_isFunctionTypeVariadic(t));
        }
        default: return nullptr;
        }
    }

    static bool has_f80(Type *t) {
        if (t->kind == Type::Float) return t->bits == 80;
        if (t->kind == Type::Array) return has_f80(t->elem);
        if (t->kind == Type::Struct)
            for (auto &f : t->st->fields)
                if (has_f80(f.type)) return true;
        return false;
    }

    StructInfo *record(CXCursor decl) {
        std::string usr = str(clang_getCursorUSR(clang_getCanonicalCursor(decl)));
        auto it = records.find(usr);
        if (it != records.end()) return it->second;
        auto *st = new StructInfo;
        records[usr] = st;
        record_usrs()[st] = usr;
        if (!clang_Cursor_isAnonymous(decl)) st->name = str(clang_getCursorSpelling(decl));
        CXCursor def = clang_getCursorDefinition(decl);
        if (clang_Cursor_isNull(def)) return st;  // opaque (FILE's internals, ...)
        st->is_union = def.kind == CXCursor_UnionDecl;
        CXType rt = clang_getCursorType(def);
        long long size = clang_Type_getSizeOf(rt), align = clang_Type_getAlignOf(rt);
        if (size < 0 || align < 0) return st;
        struct Ctx { Importer *im; StructInfo *st; };
        Ctx cx{this, st};
        clang_visitChildren(def, [](CXCursor f, CXCursor, CXClientData d) {
            Ctx *cx = (Ctx *)d;
            if (f.kind != CXCursor_FieldDecl) return CXChildVisit_Continue;
            long long off = clang_Cursor_getOffsetOfField(f);  // in bits
            Type *ft = cx->im->conv(clang_getCursorType(f));
            if (off < 0 || !ft) return CXChildVisit_Continue;
            std::string name = str(clang_getCursorSpelling(f));
            if (clang_Cursor_isBitField(f)) {
                int width = clang_getFieldDeclBitWidth(f);
                if (!name.empty() && width > 0) {  // unnamed ones are only padding
                    Field fd{name, ft, (uint64_t)off / 8};
                    fd.bit_off = off;
                    fd.bit_width = width;
                    cx->st->fields.push_back(fd);
                }
                return CXChildVisit_Continue;
            }
            if (clang_Cursor_isAnonymousRecordDecl(clang_getTypeDeclaration(clang_getCursorType(f))) && ft->kind == Type::Struct) {
                // members of an anonymous struct/union are members of the outer one
                for (auto sub : ft->st->fields) {
                    sub.offset += off / 8;
                    if (sub.bit_width) sub.bit_off += off;
                    cx->st->fields.push_back(sub);
                }
                return CXChildVisit_Continue;
            }
            cx->st->fields.push_back({name, ft, (uint64_t)off / 8});
            return CXChildVisit_Continue;
        }, &cx);
        st->size = size;
        st->align = align;
        st->complete = true;
        return st;
    }

    void top(CXCursor cur) {
        std::string name = str(clang_getCursorSpelling(cur));
        switch (cur.kind) {
        case CXCursor_FunctionDecl:
            if (clang_getCursorLinkage(cur) == CXLinkage_External && !globals.count(name))
                if (Type *ft = conv(clang_getCursorType(cur))) global(Decl::Fn, name, ft);
            break;
        case CXCursor_VarDecl:
            if (clang_getCursorLinkage(cur) == CXLinkage_External && !globals.count(name))
                if (Type *t = conv(clang_getCursorType(cur))) global(Decl::Var, name, t);
            break;
        case CXCursor_TypedefDecl:
            if (!types.count(name))
                if (Type *t = conv(clang_getTypedefDeclUnderlyingType(cur))) type(name, t);
            break;
        case CXCursor_StructDecl: case CXCursor_UnionDecl:
            if (!clang_Cursor_isAnonymous(cur) && !types.count(name)) type(name, t_struct(record(cur)));
            break;
        case CXCursor_EnumDecl: {
            Type *it = conv(clang_getEnumDeclIntegerType(cur));
            if (!it) break;
            if (!clang_Cursor_isAnonymous(cur) && !name.empty() && !types.count(name)) type(name, it);
            struct Ctx { Importer *im; Type *t; };
            Ctx cx{this, it};
            clang_visitChildren(cur, [](CXCursor e, CXCursor, CXClientData d) {
                Ctx *cx = (Ctx *)d;
                if (e.kind == CXCursor_EnumConstantDecl)
                    cx->im->global(Decl::Int, str(clang_getCursorSpelling(e)), cx->t,
                                   cx->t->sign ? (uint64_t)clang_getEnumConstantDeclValue(e) : clang_getEnumConstantDeclUnsignedValue(e));
                return CXChildVisit_Continue;
            }, &cx);
            break;
        }
        case CXCursor_MacroDefinition:
            if (!clang_Cursor_isMacroBuiltin(cur) && !clang_Cursor_isMacroFunctionLike(cur) && plain_macro(cur)) macros.push_back(name);
            break;
        default: break;
        }
    }

    // a macro whose body could be an expression: not empty, no { } ;
    bool plain_macro(CXCursor cur) {
        CXTranslationUnit tu = clang_Cursor_getTranslationUnit(cur);
        CXToken *toks;
        unsigned n;
        clang_tokenize(tu, clang_getCursorExtent(cur), &toks, &n);
        bool ok = n > 1;
        for (unsigned i = 1; ok && i < n; i++) {
            std::string s = str(clang_getTokenSpelling(tu, toks[i]));
            if (s == "{" || s == "}" || s == ";" || s == "#") ok = false;
        }
        clang_disposeTokens(tu, toks, n);
        return ok;
    }

    // the macros' values: a second parse declares a constant per macro and
    // lets clang evaluate it; macros that are not constant expressions fail
    // there and are skipped
    void macro_values(CXIndex idx, const std::string &includes, const std::vector<const char *> &args) {
        std::string src = includes;
        for (auto &m : macros)
            if (!globals.count(m) && !types.count(m))
                src += "static const __typeof__(" + m + ") " + MACRO_PREFIX + m + " = " + m + ";\n";
        CXUnsavedFile f{"lisc_macros.c", src.c_str(), (unsigned long)src.size()};
        CXTranslationUnit tu = clang_parseTranslationUnit(idx, "lisc_macros.c", args.data(), (int)args.size(), &f, 1,
                                                          CXTranslationUnit_SkipFunctionBodies);
        if (!tu) return;
        clang_visitChildren(clang_getTranslationUnitCursor(tu), [](CXCursor cur, CXCursor, CXClientData d) {
            Importer *im = (Importer *)d;
            if (cur.kind != CXCursor_VarDecl) return CXChildVisit_Continue;
            std::string name = str(clang_getCursorSpelling(cur));
            if (name.compare(0, strlen(MACRO_PREFIX), MACRO_PREFIX)) return CXChildVisit_Continue;
            name = name.substr(strlen(MACRO_PREFIX));
            CXEvalResult r = clang_Cursor_Evaluate(cur);
            if (!r) return CXChildVisit_Continue;
            Type *t = im->conv(clang_getCursorType(cur));
            switch (clang_EvalResult_getKind(r)) {
            case CXEval_Int:
                if (t && t->is_int())
                    im->global(Decl::Int, name, t, clang_EvalResult_isUnsignedInt(r) ? clang_EvalResult_getAsUnsigned(r)
                                                                                      : (uint64_t)clang_EvalResult_getAsLongLong(r));
                break;
            case CXEval_Float:
                if (t && t->is_float()) im->global(Decl::Float, name, t, 0, clang_EvalResult_getAsDouble(r));
                break;
            case CXEval_StrLiteral:
                im->global(Decl::Str, name, nullptr, 0, 0, clang_EvalResult_getAsStr(r));
                break;
            default: break;
            }
            clang_EvalResult_dispose(r);
            return CXChildVisit_Continue;
        }, this);
        clang_disposeTranslationUnit(tu);
    }
};

// ------------------------------------------------------------------ cache

// a file clang read, as it was then
struct Dep {
    std::string path;
    long long mtime = 0, size = 0;
};
bool stat_dep(Dep &d) {
    struct stat sb;
    if (stat(d.path.c_str(), &sb) != 0) return false;
    d.mtime = (long long)sb.st_mtim.tv_sec * 1000000000LL + sb.st_mtim.tv_nsec;
    d.size = sb.st_size;
    return true;
}

std::string cache_dir() {
    const char *x = getenv("XDG_CACHE_HOME"), *h = getenv("HOME");
    if (x && *x) return std::string(x) + "/lisc";
    if (h && *h) return std::string(h) + "/.cache/lisc";
    return "";
}

// what an import depends on besides the files: the source, the flags and lisc itself
std::string cache_key(const std::string &src, const std::vector<const char *> &args) {
    std::string k = "v1\n";
    Dep self{"/proc/self/exe"};
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) self.path.assign(exe, n);
    stat_dep(self);
    k += self.path + " " + std::to_string(self.mtime) + " " + std::to_string(self.size) + "\n";
    for (auto a : args) k += std::string(a) + "\n";
    return k + src;
}

std::string hex(const std::string &s) {
    static const char *d = "0123456789abcdef";
    std::string r;
    for (unsigned char ch : s) r += d[ch >> 4], r += d[ch & 15];
    return r.empty() ? "-" : r;
}
std::string unhex(const std::string &s) {
    std::string r;
    if (s == "-") return r;
    auto v = [](char ch) { return ch <= '9' ? ch - '0' : ch - 'a' + 10; };
    for (size_t i = 0; i + 1 < s.size(); i += 2) r += (char)(v(s[i]) << 4 | v(s[i + 1]));
    return r;
}

struct Writer {
    std::map<StructInfo *, int> ids;
    std::vector<StructInfo *> order;
    std::string type(Type *t) {
        switch (t->kind) {
        case Type::Void: return "v";
        case Type::Bool: return "b";
        case Type::Int: return (t->sign ? "i" : "u") + std::to_string(t->bits);
        case Type::Float: return "f" + std::to_string(t->bits);
        case Type::Ptr: return "p " + type(t->elem);
        case Type::Array: return "a " + std::to_string(t->n) + " " + type(t->elem);
        case Type::Simd: return "V " + std::to_string(t->n) + " " + type(t->elem);
        case Type::Struct: {
            auto it = ids.find(t->st);
            if (it == ids.end()) {
                it = ids.emplace(t->st, (int)order.size()).first;
                order.push_back(t->st);
            }
            return "s" + std::to_string(it->second);
        }
        case Type::Fn: {
            std::string r = "F " + std::to_string((int)t->variadic) + " " + std::to_string(t->params.size()) + " " + type(t->elem);
            for (Type *p : t->params) r += " " + type(p);
            return r;
        }
        }
        return "v";
    }
};

bool save(const std::string &path, const std::string &key, const std::vector<Dep> &deps, const std::vector<Decl> &decls) {
    Writer w;
    std::ostringstream body;
    for (auto &d : decls) {
        static const char *k[] = {"fn", "var", "type", "int", "float", "str"};
        body << k[d.kind] << " " << d.name;
        switch (d.kind) {
        case Decl::Int: body << " " << d.i << " " << w.type(d.t); break;
        case Decl::Float: { uint64_t bits; memcpy(&bits, &d.f, 8); body << " " << bits << " " << w.type(d.t); break; }
        case Decl::Str: body << " " << hex(d.s); break;
        default: body << " " << w.type(d.t); break;
        }
        body << "\n";
    }
    // the records, and any they lead to (the list grows while it is written)
    std::ostringstream recs;
    for (size_t i = 0; i < w.order.size(); i++) {
        StructInfo *st = w.order[i];
        recs << "rec " << i << " " << hex(record_usrs()[st]) << " " << hex(st->name) << " " << st->is_union << " "
             << st->complete << " " << st->size << " " << st->align << " " << st->fields.size() << "\n";
        for (auto &f : st->fields)
            recs << "fld " << f.name << " " << f.offset << " " << f.bit_off << " " << f.bit_width << " " << w.type(f.type) << "\n";
    }
    std::string tmp = path + ".tmp" + std::to_string(getpid());
    {
        std::ofstream out(tmp);
        if (!out) return false;
        out << "lisc-c-cache " << hex(key) << "\n";
        for (auto &d : deps) out << "dep " << d.mtime << " " << d.size << " " << hex(d.path) << "\n";
        out << "records " << w.order.size() << "\n" << recs.str() << body.str();
        if (!out) return false;
    }
    return rename(tmp.c_str(), path.c_str()) == 0;
}

struct Reader {
    std::istringstream in;
    std::vector<StructInfo *> recs;
    bool ok = true;
    Type *type() {
        std::string t;
        if (!(in >> t)) { ok = false; return t_void(); }
        switch (t[0]) {
        case 'v': return t_void();
        case 'b': return t_bool();
        case 'i': return t_int((unsigned)strtoul(t.c_str() + 1, nullptr, 10), true);
        case 'u': return t_int((unsigned)strtoul(t.c_str() + 1, nullptr, 10), false);
        case 'f': return t_float((unsigned)strtoul(t.c_str() + 1, nullptr, 10));
        case 'p': return t_ptr(type());
        case 'a': { uint64_t n = 0; in >> n; return t_array(type(), n); }
        case 'V': { uint64_t n = 0; in >> n; return t_simd(type(), n); }
        case 's': {
            size_t id = strtoul(t.c_str() + 1, nullptr, 10);
            if (id >= recs.size()) { ok = false; return t_void(); }
            return t_struct(recs[id]);
        }
        case 'F': {
            int var = 0;
            size_t n = 0;
            in >> var >> n;
            Type *ret = type();
            std::vector<Type *> ps;
            for (size_t i = 0; i < n && ok; i++) ps.push_back(type());
            return t_fn(ret, ps, var != 0);
        }
        }
        ok = false;
        return t_void();
    }
};

// the cached declarations, if the cache has this import and none of its files changed
bool load(const std::string &path, const std::string &key, std::vector<Decl> &decls) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    Reader r;
    r.in.str(ss.str());
    std::string word, h;
    if (!(r.in >> word >> h) || word != "lisc-c-cache" || unhex(h) != key) return false;
    size_t nrec = 0;
    while (r.in >> word && word == "dep") {
        Dep d, now;
        r.in >> d.mtime >> d.size >> h;
        now.path = unhex(h);
        if (!stat_dep(now) || now.mtime != d.mtime || now.size != d.size) return false;
    }
    if (word != "records" || !(r.in >> nrec)) return false;
    // records first by USR (shared with every other C module), filled in below if new here
    struct Rec { StructInfo *st; bool fill; std::string name; int un, complete; uint64_t size, align; size_t nf; std::streampos at; };
    std::vector<Rec> rs;
    for (size_t i = 0; i < nrec; i++) {
        Rec rc{};
        size_t id;
        std::string usr, name;
        if (!(r.in >> word >> id >> usr >> name >> rc.un >> rc.complete >> rc.size >> rc.align >> rc.nf) || word != "rec") return false;
        rc.name = unhex(name);
        usr = unhex(usr);
        auto &all = all_records();
        auto it = all.find(usr);
        if (it != all.end()) rc.st = it->second;
        else {
            rc.st = new StructInfo;
            all[usr] = rc.st;
            record_usrs()[rc.st] = usr;
            rc.fill = true;
        }
        r.recs.push_back(rc.st);
        std::string line;
        std::getline(r.in, line);
        rc.at = r.in.tellg();
        for (size_t j = 0; j < rc.nf; j++) std::getline(r.in, line);  // (fields read once every record has its id)
        rs.push_back(rc);
    }
    std::streampos decl_at = r.in.tellg();
    for (auto &rc : rs) {
        if (!rc.fill) continue;
        r.in.seekg(rc.at);
        StructInfo *st = rc.st;
        st->name = rc.name;
        st->is_union = rc.un;
        for (size_t j = 0; j < rc.nf && r.ok; j++) {
            Field fd{};
            if (!(r.in >> word >> fd.name >> fd.offset >> fd.bit_off >> fd.bit_width) || word != "fld") return false;
            fd.type = r.type();
            st->fields.push_back(fd);
        }
        st->size = rc.size;
        st->align = rc.align;
        st->complete = rc.complete;
    }
    r.in.seekg(decl_at);
    while (r.ok && r.in >> word) {
        Decl d{};
        r.in >> d.name;
        if (word == "fn") d.kind = Decl::Fn, d.t = r.type();
        else if (word == "var") d.kind = Decl::Var, d.t = r.type();
        else if (word == "type") d.kind = Decl::TypeName, d.t = r.type();
        else if (word == "int") { d.kind = Decl::Int; r.in >> d.i; d.t = r.type(); }
        else if (word == "float") { uint64_t bits = 0; d.kind = Decl::Float; r.in >> bits; memcpy(&d.f, &bits, 8); d.t = r.type(); }
        else if (word == "str") { d.kind = Decl::Str; r.in >> h; d.s = unhex(h); }
        else return false;
        decls.push_back(d);
    }
    return r.ok;
}

// the declarations go to the compiler, under the C module's name
void apply_decls(Compiler &c, const std::vector<Decl> &decls) {
    for (auto &d : decls) switch (d.kind) {
        case Decl::Fn: c.add_extern_fn(d.name, d.t); break;
        case Decl::Var: c.add_extern_var(d.name, d.t); break;
        case Decl::TypeName: if (!c.has_type(d.name)) c.add_type(d.name, d.t); break;
        case Decl::Int: c.add_const_int(d.name, d.t, d.i); break;
        case Decl::Float: c.add_const_float(d.name, d.t, d.f); break;
        case Decl::Str: c.add_const_str(d.name, d.s); break;
        }
}

uint64_t fnv(const std::string &s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char ch : s) h = (h ^ ch) * 1099511628211ULL;
    return h;
}

}  // namespace

void c_include(Compiler &c, const std::vector<std::string> &headers, Loc at) {
    std::string src;
    for (auto &h : headers) src += "#include <" + h + ">\n";
    std::vector<const char *> args = {"-x", "c", "-std=gnu11", "-w", "-ferror-limit=0", "-resource-dir", CLANG_RESOURCE_DIR};
    std::vector<std::string> incs;
    for (auto &d : c.include_dirs) incs.push_back("-I" + d);
    for (auto &i : incs) args.push_back(i.c_str());

    std::string key = cache_key(src, args), dir = c.c_cache ? cache_dir() : "", path;
    if (!dir.empty()) {
        char name[32];
        snprintf(name, sizeof name, "/%016llx.c-import", (unsigned long long)fnv(key));
        path = dir + name;
        std::vector<Decl> decls;
        if (load(path, key, decls)) {
            apply_decls(c, decls);
            return;
        }
    }

    CXIndex idx = clang_createIndex(0, 0);
    CXUnsavedFile f{"lisc_import.c", src.c_str(), (unsigned long)src.size()};
    CXTranslationUnit tu = clang_parseTranslationUnit(idx, "lisc_import.c", args.data(), (int)args.size(), &f, 1,
                                                      CXTranslationUnit_DetailedPreprocessingRecord | CXTranslationUnit_SkipFunctionBodies);
    if (!tu) fatal(at, "libclang could not parse the headers");
    for (unsigned i = 0; i < clang_getNumDiagnostics(tu); i++) {
        CXDiagnostic d = clang_getDiagnostic(tu, i);
        if (clang_getDiagnosticSeverity(d) >= CXDiagnostic_Error) {
            std::string msg = str(clang_formatDiagnostic(d, clang_defaultDiagnosticDisplayOptions()));
            fatal(at, "in the C headers: %s", msg.c_str());
        }
        clang_disposeDiagnostic(d);
    }
    Importer im{at};
    clang_visitChildren(clang_getTranslationUnitCursor(tu), [](CXCursor cur, CXCursor, CXClientData d) {
        ((Importer *)d)->top(cur);
        return CXChildVisit_Continue;
    }, &im);
    // every file the headers brought in: the cache is good while they stay the same
    std::vector<Dep> deps;
    clang_getInclusions(tu, [](CXFile f, CXSourceLocation *, unsigned, CXClientData d) {
        Dep dep{str(clang_getFileName(f))};
        if (!dep.path.empty() && dep.path != "lisc_import.c") ((std::vector<Dep> *)d)->push_back(dep);
    }, &deps);
    clang_disposeTranslationUnit(tu);
    im.macro_values(idx, src, args);
    clang_disposeIndex(idx);
    apply_decls(c, im.decls);

    if (path.empty()) return;
    for (auto &d : deps)
        if (!stat_dep(d)) return;  // (something clang read is gone already: don't cache)
    mkdir(dir.substr(0, dir.rfind('/')).c_str(), 0755);
    mkdir(dir.c_str(), 0755);
    save(path, key, deps, im.decls);
}
