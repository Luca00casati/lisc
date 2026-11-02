// type.h — lisc types: C's types, with C struct layout.
//
//   i8 i16 i32 i64  u8 u16 u32 u64  f32 f64  bool  void
//   (ptr T)  (array T N)  (fn (A B ...) R)  struct names
//
// Types are interned: two equal types are the same pointer.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace llvm { class Type; class StructType; }

struct Type;

struct Field {
    std::string name;
    Type *type;
    uint64_t offset;       // bytes (for a bit field: the byte holding its first bit)
    uint64_t bit_off = 0;  // bit field: first bit, counted from the struct's start
    unsigned bit_width = 0;  // 0 = an ordinary field
};

struct StructInfo {
    std::string name;
    bool packed = false;       // (struct name :packed ...): no padding, alignment 1
    uint64_t min_align = 1;    // (struct name :align N ...)
    Type *slice_of = nullptr;  // (slice T): data (ptr T), len u64
    std::vector<Field> fields;
    uint64_t size = 0, align = 1;
    bool complete = false;
    bool is_union = false;
    llvm::Type *ll = nullptr;  // built by the compiler once complete
    const Field *find(const std::string &n) const {
        for (auto &f : fields)
            if (f.name == n) return &f;
        return nullptr;
    }
};

// (enum name T a b (c 5) ...): a distinct integer type with named values
struct EnumInfo {
    std::string name;
    std::vector<std::pair<std::string, uint64_t>> values;
};

struct Type {
    enum Kind { Void, Bool, Int, Float, Ptr, Array, Struct, Fn } kind;
    unsigned bits = 0;   // Int, Float
    bool sign = false;   // Int
    Type *elem = nullptr;  // Ptr, Array: element; Fn: result
    uint64_t n = 0;        // Array: length
    StructInfo *st = nullptr;
    std::vector<Type *> params;  // Fn
    bool variadic = false;       // Fn
    EnumInfo *en = nullptr;      // Int: an enum (a type of its own, not the plain integer)

    bool is_int() const { return kind == Int; }
    bool is_float() const { return kind == Float; }
    bool is_num() const { return kind == Int || kind == Float; }
    bool is_ptr() const { return kind == Ptr; }
    bool is_scalar() const { return kind == Int || kind == Float || kind == Ptr || kind == Bool; }
};

Type *t_void();
Type *t_bool();
Type *t_int(unsigned bits, bool sign);
Type *t_float(unsigned bits);
Type *t_ptr(Type *elem);
Type *t_array(Type *elem, uint64_t n);
Type *t_fn(Type *ret, const std::vector<Type *> &params, bool variadic);
Type *t_struct(StructInfo *st);  // one Type per StructInfo
Type *t_enum(EnumInfo *en, Type *base);  // a new integer type, distinct from base
Type *t_slice(Type *elem);       // (slice T), one per element type

uint64_t size_of(const Type *t);
uint64_t align_of(const Type *t);
// lay out the fields in order with natural alignment, bit fields included (C rules, x86-64 SysV)
void layout_struct(StructInfo *st);
std::string type_name(const Type *t);
