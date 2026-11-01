// type.cpp — type interning and C layout.

#include "type.h"

#include <map>
#include <tuple>

namespace {

Type *make(Type t) { return new Type(std::move(t)); }

}  // namespace

Type *t_void() { static Type *t = make({Type::Void}); return t; }
Type *t_bool() { static Type *t = make({Type::Bool, 1}); return t; }

Type *t_int(unsigned bits, bool sign) {
    static std::map<std::pair<unsigned, bool>, Type *> m;
    Type *&t = m[{bits, sign}];
    if (!t) { t = make({Type::Int, bits}); t->sign = sign; }
    return t;
}

Type *t_float(unsigned bits) {
    static std::map<unsigned, Type *> m;
    Type *&t = m[bits];
    if (!t) t = make({Type::Float, bits});
    return t;
}

Type *t_ptr(Type *elem) {
    static std::map<Type *, Type *> m;
    Type *&t = m[elem];
    if (!t) { t = make({Type::Ptr, 64}); t->elem = elem; }
    return t;
}

Type *t_array(Type *elem, uint64_t n) {
    static std::map<std::pair<Type *, uint64_t>, Type *> m;
    Type *&t = m[{elem, n}];
    if (!t) { t = make({Type::Array}); t->elem = elem; t->n = n; }
    return t;
}

Type *t_fn(Type *ret, const std::vector<Type *> &params, bool variadic) {
    static std::map<std::tuple<Type *, std::vector<Type *>, bool>, Type *> m;
    Type *&t = m[{ret, params, variadic}];
    if (!t) { t = make({Type::Fn}); t->elem = ret; t->params = params; t->variadic = variadic; }
    return t;
}

Type *t_struct(StructInfo *st) {
    static std::map<StructInfo *, Type *> m;
    Type *&t = m[st];
    if (!t) { t = make({Type::Struct}); t->st = st; }
    return t;
}

uint64_t size_of(const Type *t) {
    switch (t->kind) {
    case Type::Void: return 1;
    case Type::Bool: return 1;
    case Type::Int: case Type::Float: return t->bits / 8;
    case Type::Ptr: case Type::Fn: return 8;
    case Type::Array: return t->n * size_of(t->elem);
    case Type::Struct: return t->st->size;
    }
    return 0;
}

uint64_t align_of(const Type *t) {
    switch (t->kind) {
    case Type::Array: return align_of(t->elem);
    case Type::Struct: return t->st->align;
    default: return size_of(t);
    }
}

void layout_struct(StructInfo *st) {
    uint64_t off = 0, align = 1;
    for (auto &f : st->fields) {
        uint64_t a = align_of(f.type);
        if (a > align) align = a;
        if (st->is_union) {
            f.offset = 0;
            if (size_of(f.type) > off) off = size_of(f.type);
        } else {
            off = (off + a - 1) / a * a;
            f.offset = off;
            off += size_of(f.type);
        }
    }
    st->align = align;
    st->size = (off + align - 1) / align * align;
    st->complete = true;
}

std::string type_name(const Type *t) {
    switch (t->kind) {
    case Type::Void: return "void";
    case Type::Bool: return "bool";
    case Type::Int: return (t->sign ? "i" : "u") + std::to_string(t->bits);
    case Type::Float: return "f" + std::to_string(t->bits);
    case Type::Ptr: return "(ptr " + type_name(t->elem) + ")";
    case Type::Array: return "(array " + type_name(t->elem) + " " + std::to_string(t->n) + ")";
    case Type::Struct: return t->st->name.empty() ? "<anonymous struct>" : t->st->name;
    case Type::Fn: {
        std::string s = "(fn (";
        for (size_t i = 0; i < t->params.size(); i++) s += (i ? " " : "") + type_name(t->params[i]);
        if (t->variadic) s += t->params.empty() ? "..." : " ...";
        return s + ") " + type_name(t->elem) + ")";
    }
    }
    return "?";
}
