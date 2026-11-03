// type.cpp — type interning and C layout.

#include "type.h"

#include <algorithm>
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

Type *t_simd(Type *elem, uint64_t n) {
    static std::map<std::pair<Type *, uint64_t>, Type *> m;
    Type *&t = m[{elem, n}];
    if (!t) { t = make({Type::Simd}); t->elem = elem; t->n = n; }
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

Type *t_enum(EnumInfo *en, Type *base) {
    Type *t = make({Type::Int, base->bits});
    t->sign = base->sign;
    t->en = en;
    return t;
}

Type *t_slice(Type *elem) {
    static std::map<Type *, Type *> m;
    Type *&t = m[elem];
    if (!t) {
        auto *st = new StructInfo;
        st->name = "(slice " + type_name(elem) + ")";
        st->slice_of = elem;
        st->fields = {{"data", t_ptr(elem), 0}, {"len", t_int(64, false), 8}};
        st->size = 16;
        st->align = 8;
        st->complete = true;
        t = t_struct(st);
    }
    return t;
}

Type *t_closure(Type *fn) {
    static std::map<Type *, Type *> m;
    Type *&t = m[fn];
    if (!t) {
        auto *st = new StructInfo;
        std::string ps;
        for (Type *p : fn->params) ps += (ps.empty() ? "" : " ") + type_name(p);
        st->name = "(closure (" + ps + ") " + type_name(fn->elem) + ")";
        st->closure_of = fn;
        std::vector<Type *> with_env{t_ptr(t_void())};
        with_env.insert(with_env.end(), fn->params.begin(), fn->params.end());
        st->fields = {{"fn", t_ptr(t_fn(fn->elem, with_env, false)), 0}, {"env", t_ptr(t_void()), 8}};
        st->size = 16;
        st->align = 8;
        st->complete = true;
        t = t_struct(st);
    }
    return t;
}

uint64_t size_of(const Type *t) {
    switch (t->kind) {
    case Type::Void: return 1;
    case Type::Bool: return 1;
    case Type::Int: return t->bits / 8;
    case Type::Float: return t->bits == 80 ? 16 : t->bits / 8;  // (C's long double: 10 bytes used, 16 with padding)
    case Type::Ptr: case Type::Fn: return 8;
    case Type::Array: case Type::Simd: return t->n * size_of(t->elem);
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
    uint64_t bits = 0, align = 1;  // position in bits, so bit fields can share bytes
    auto up = [](uint64_t v, uint64_t a) { return (v + a - 1) / a * a; };
    for (auto &f : st->fields) {
        uint64_t a = st->packed ? 1 : align_of(f.type), unit = 8 * size_of(f.type);
        if (a > align) align = a;
        if (st->is_union) {
            f.offset = f.bit_off = 0;
            bits = std::max(bits, f.bit_width ? up(f.bit_width, 8) : unit);
        } else if (f.bit_width) {
            // a bit field never straddles a boundary of its type's size (packed: it may)
            if (!st->packed && bits / unit != (bits + f.bit_width - 1) / unit) bits = up(bits, unit);
            f.bit_off = bits;
            f.offset = bits / 8;
            bits += f.bit_width;
        } else {
            bits = up(bits, 8 * a);
            f.offset = bits / 8;
            bits += unit;
        }
    }
    if (st->min_align > align) align = st->min_align;
    st->align = align;
    st->size = up(up(bits, 8) / 8, align);
    st->complete = true;
}

std::string type_name(const Type *t) {
    switch (t->kind) {
    case Type::Void: return "void";
    case Type::Bool: return "bool";
    case Type::Int: return t->en ? t->en->name : (t->sign ? "i" : "u") + std::to_string(t->bits);
    case Type::Float: return "f" + std::to_string(t->bits);
    case Type::Ptr: return "(ptr " + type_name(t->elem) + ")";
    case Type::Array: return "(array " + type_name(t->elem) + " " + std::to_string(t->n) + ")";
    case Type::Simd: return "(simd " + type_name(t->elem) + " " + std::to_string(t->n) + ")";
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
