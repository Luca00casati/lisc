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
// Types lisc has no equivalent for (long double, __int128, vectors) are
// skipped; a declaration that needs one is left out. Bit fields are kept.

#include <clang-c/Index.h>

#include <cstring>
#include <map>
#include <set>

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

struct Importer {
    Compiler &c;
    Loc at;
    std::map<std::string, StructInfo *> &records = all_records();  // by USR, shared by every C module
    std::vector<std::string> macros;

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
        case CXType_Enum: return conv(clang_getEnumDeclIntegerType(clang_getTypeDeclaration(t)));
        case CXType_Record: return t_struct(record(clang_getTypeDeclaration(t)));
        case CXType_FunctionProto: case CXType_FunctionNoProto: {
            Type *ret = conv(clang_getResultType(t));
            if (!ret) return nullptr;
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

    StructInfo *record(CXCursor decl) {
        std::string usr = str(clang_getCursorUSR(clang_getCanonicalCursor(decl)));
        auto it = records.find(usr);
        if (it != records.end()) return it->second;
        auto *st = new StructInfo;
        records[usr] = st;
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
            if (clang_getCursorLinkage(cur) == CXLinkage_External && !c.has_global(name))
                if (Type *ft = conv(clang_getCursorType(cur))) c.add_extern_fn(name, ft);
            break;
        case CXCursor_VarDecl:
            if (clang_getCursorLinkage(cur) == CXLinkage_External && !c.has_global(name))
                if (Type *t = conv(clang_getCursorType(cur))) c.add_extern_var(name, t);
            break;
        case CXCursor_TypedefDecl:
            if (!c.has_type(name))
                if (Type *t = conv(clang_getTypedefDeclUnderlyingType(cur))) c.add_type(name, t);
            break;
        case CXCursor_StructDecl: case CXCursor_UnionDecl:
            if (!clang_Cursor_isAnonymous(cur) && !c.has_type(name)) c.add_type(name, t_struct(record(cur)));
            break;
        case CXCursor_EnumDecl: {
            Type *it = conv(clang_getEnumDeclIntegerType(cur));
            if (!it) break;
            if (!clang_Cursor_isAnonymous(cur) && !name.empty() && !c.has_type(name)) c.add_type(name, it);
            struct Ctx { Importer *im; Type *t; };
            Ctx cx{this, it};
            clang_visitChildren(cur, [](CXCursor e, CXCursor, CXClientData d) {
                Ctx *cx = (Ctx *)d;
                if (e.kind == CXCursor_EnumConstantDecl)
                    cx->im->c.add_const_int(str(clang_getCursorSpelling(e)), cx->t,
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
            if (!c.has_global(m) && !c.has_type(m))
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
                    im->c.add_const_int(name, t, clang_EvalResult_isUnsignedInt(r) ? clang_EvalResult_getAsUnsigned(r)
                                                                                   : (uint64_t)clang_EvalResult_getAsLongLong(r));
                break;
            case CXEval_Float:
                if (t && t->is_float()) im->c.add_const_float(name, t, clang_EvalResult_getAsDouble(r));
                break;
            case CXEval_StrLiteral:
                im->c.add_const_str(name, clang_EvalResult_getAsStr(r));
                break;
            default: break;
            }
            clang_EvalResult_dispose(r);
            return CXChildVisit_Continue;
        }, this);
        clang_disposeTranslationUnit(tu);
    }
};

}  // namespace

void c_include(Compiler &c, const std::vector<std::string> &headers, Loc at) {
    std::string src;
    for (auto &h : headers) src += "#include <" + h + ">\n";
    std::vector<const char *> args = {"-x", "c", "-std=gnu11", "-w", "-ferror-limit=0", "-resource-dir", CLANG_RESOURCE_DIR};
    std::vector<std::string> incs;
    for (auto &d : c.include_dirs) incs.push_back("-I" + d);
    for (auto &i : incs) args.push_back(i.c_str());
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
    Importer im{c, at, all_records()};
    clang_visitChildren(clang_getTranslationUnitCursor(tu), [](CXCursor cur, CXCursor, CXClientData d) {
        ((Importer *)d)->top(cur);
        return CXChildVisit_Continue;
    }, &im);
    clang_disposeTranslationUnit(tu);
    im.macro_values(idx, src, args);
    clang_disposeIndex(idx);
}
