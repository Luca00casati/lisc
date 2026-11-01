# lisc

A compiled language with Lisp syntax and C's types. Programs are
s-expressions; the compiler (C++, LLVM 18) type-checks them and emits
optimised native code linked against libc. C headers are imported directly
through libclang, so libc and kernel interfaces need no hand-written bindings.

```lisp
(c-include "stdio.h" "fcntl.h" "sys/ioctl.h" "drm/i915_drm.h")

(defn main () i32
  (let ((fd (open "/dev/dri/renderD128" O_RDWR))
        (id i32 0)
        (gp (make drm_i915_getparam :param I915_PARAM_CHIPSET_ID :value (addr id))))
    (when (= (ioctl fd DRM_IOCTL_I915_GETPARAM (addr gp)) 0)
      (printf "i915 chip id 0x%04x\n" id)))
  0)
```

## Build

    make            # needs llvm-18 and libclang-18 (headers + libraries)
    make test
    ./lisc prog.lsp [more.c|.o ...] [-o prog] [-O0..-O3] [-I dir] [--emit-llvm] [-c] [-lname ...]

Extra `.c`/`.o` inputs are linked in. `(c-include ...)` searches the source
file's directory and `-I` directories, then the system ones.

## Language (core, phases 1-2)

Types: `i8 i16 i32 i64 u8 u16 u32 u64 isize usize f32 f64 bool void`,
`(ptr T)`, `(array T N)`, `(fn (A B ...) R)`, struct/union names, and every
typedef/struct a `c-include` brings in.

Top level:

| form | |
|---|---|
| `(c-include "h" ...)` | functions, extern variables, structs (clang's exact layout), typedefs, enum constants and constant `#define`s of these headers |
| `(struct name (field T) ...)` `(union ...)` | C layout; `(field T :bits N)` is a bit field |
| `(const NAME expr)` | compile-time constant; untyped if `expr` is |
| `(def name T [init])` | global variable |
| `(defn name ((a T) ...) R body...)` | function; the last expression is the result |
| `(extern name (T ... [...]) R)` | a C function by hand |

Expressions — everything is one; statements have type `void`:

    (do e...)  (let ((x init) (y T init) (z T)) body...)  (set place e)
    (if c a [b])  (when c body...)  (unless c body...)
    (while c body...)  (for (i start end [step]) body...)  (break)  (continue)  (return [e])
    (+ - * / % & | ^ << >>)  (- x)  (~ x)  (= != < <= > >=)  (and ...)  (or ...)  (not x)
    (. x field ...) — also written x.field.sub; pointers are followed automatically
    (aref a i)  (deref p)  (addr place)  (cast T e)  (bitcast T e)  (sizeof T)  (alignof T)
    (make T :field e ...)  (make (array T N) e ...)  — the rest is zero
    "strings" are (ptr i8);  true false null

Typing rules:

- Literals are untyped until used: `0.1` becomes exactly C's `0.1f` where an
  `f32` is expected, `0.1` (double) otherwise; integers take the type they meet.
- Operands of an operator must have one type; mixing `i32` and `f32` (or `i32`
  and `i64`) is an error — write `(cast T e)`.
- Implicit conversions only: `(ptr void)` ↔ any pointer, arrays to a pointer to
  their first element, and C's default promotions for variadic arguments.
- Conditions may be `bool`, integers or pointers.
- Every function uses the C calling convention (x86-64 System V), lisc's own
  included: structs pass and return by value to and from C, and C can call
  lisc functions through function pointers.
- Bit fields (imported or declared) read and write like other fields; only
  `(addr ...)` of one is an error.
- Floating point is strict IEEE (no fast-math, no FMA contraction), so results
  match a C build with `-ffp-contract=off`.

## Status

Done: reader, typed core, LLVM code generation and optimisation, C header
import. Next: macros (a compile-time Lisp interpreter with quasiquote that can
ask for the type of an expression), operator overloading by type (`defop`),
then porting the teapot ray tracer to lisc.
