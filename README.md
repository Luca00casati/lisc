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
    ./lisc prog.lsp [more.c|.o ...] [-o prog] [-O0..-O3] [-I dir] [-D NAME[=value]] [--emit-llvm] [-c] [-lname ...]

Extra `.c`/`.o` inputs are linked in. `(c-include ...)` searches the source
file's directory and `-I` directories, then the system ones.

## Language

Types: `i8 i16 i32 i64 u8 u16 u32 u64 isize usize f32 f64 bool void`,
`(ptr T)`, `(array T N)`, `(fn (A B ...) R)`, struct/union names, and every
typedef/struct a `c-include` brings in.

Top level:

| form | |
|---|---|
| `(import "mod" [:as m] [:all])` | a module: its names as `mod::name` (see below) |
| `(include "file.lsp" ...)` | another file's forms as part of this module |
| `(c-include "h" ...)` | functions, extern variables, structs (clang's exact layout), typedefs, enum constants and constant `#define`s of these headers |
| `(struct name (field T) ...)` `(union ...)` | C layout; `(field T :bits N)` is a bit field |
| `(const NAME expr)` | compile-time constant; untyped if `expr` is |
| `(def name T [init])` | global variable |
| `(defn name ((a T) ...) R body...)` | function; the last expression is the result |
| `(extern name (T ... [...]) R)` | a C function by hand |
| `(defop op ((a T) ...) R body...)` | an operator for these operand types |
| `(defmacro name (params...) body...)` | a macro (see below) |
| `(defmeta name (params...) body...)` | a compile-time helper function for macros |
| `(defn- ...)` `(def- ...)` `(const- ...)` `(struct- ...)` `(defmacro- ...)` `(private form ...)` | names only their module can use |

Expressions — everything is one; statements have type `void`:

    (do e...)  (let ((x init) (y T init) (z T)) body...)  (set place e)
    (if c a [b])  (when c body...)  (unless c body...)
    (while c body...)  (for (i [T] start end [step]) body...)  (break)  (continue)  (return [e])
    (+ - * / % & | ^ << >>)  (- x)  (~ x)  (= != < <= > >=)  (and ...)  (or ...)  (not x)
    (. x field ...) — also written x.field.sub; pointers are followed automatically
    (aref a i)  (deref p)  (addr place)  (cast T e)  (bitcast T e)  (sizeof T)  (alignof T)
    (make T :field e ...)  (make (array T N) e ...)  — the rest is zero
    "strings" are (ptr i8);  true false null
    (defined NAME)  (if-defined NAME then [else]) — compile-time, like #ifdef; -D NAME[=value] defines one

Typing rules:

- Literals are untyped until used: `0.1` becomes exactly C's `0.1f` where an
  `f32` is expected, `0.1` (double) otherwise; integers take the type they meet.
- Operands of an operator must have one type. Lossless integer widening is
  implicit (as in Zig): a smaller integer into a bigger one of the same
  signedness, or unsigned into a bigger signed type; in assignments and
  arguments also `f32` into `f64`. Anything else — mixing `i32` and `u32`,
  integers and floats, narrowing — needs `(cast T e)`.
- `#define` and enum constants from C headers are untyped too, as in C.
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

## Modules

Every file is a module, named after the file. `(import "scene")` (path
relative to the importing file, `.lsp` optional) makes its names reachable as
`scene::build`; there is one level only (`a::b::c` is an error).

    (import "scene")             ; scene::build, scene::camera ...
    (import "rt-kernel" :as rt)  ; rt::build
    (import "vec" :all)          ; also dot, cross ... unqualified

A name resolves to the module's own definition first, then to the `:all`
imports (a name two of them define must be written qualified), then to C
imports and `-D` constants, which are global as in C. Definitions marked
private (`defn-`, `(private ...)` ...) are unreachable from other modules.

Macros are hygienic across modules too: the symbols of a macro's template
resolve in the macro's own module, so `(geom::doubled v)` works anywhere
even though its expansion calls geom's private helpers. Operators (`defop`)
are chosen by type, so they apply wherever their types are used.

## Operators

`(defop + ((a v3) (b v3)) v3 ...)` defines `+` for two `v3`s. An operator form
uses the definition whose parameter types match its operands exactly; a
literal operand takes the type the definition wants (`(* 2.0 v)` with
`(defop * ((k f32) (a v3)) ...)`). n-ary forms fold left: `(+ a b c)` is
`(+ (+ a b) c)`. Unary `-` and the comparisons can be defined the same way.
Definitions are ordinary functions, so the optimiser inlines them: there is no
runtime cost.

## Macros

A macro is a function that runs inside the compiler, on forms: it receives its
arguments unevaluated and returns the form to compile in its place. The
language inside is a small Lisp (see `src/macro.h` for the full list):
quasiquote, `if`/`cond`/`let`/`while`/`lambda`, list functions (`map`,
`mapcat`, `filter`, `range`, ...), `sym`/`str`/`gensym`, and parameters that
destructure (`(defmacro unroll ((i n) &body body) ...)`).

Macros can ask about types: `(type-of e)` is the type `e` has where the macro
is used, and `(type-kind T)`, `(type-elem T)`, `(type-fields T)` ... describe a
type. So a macro can dispatch on its argument's type, or generate code for every
field of a struct:

```lisp
(defmacro derive-ops (T)                 ; + - * and = for any struct, field by field
  (let ((fields (map first (type-fields T))))
    `(do ,@(map (lambda (op)
                  `(defop ,op ((a ,T) (b ,T)) ,T
                     (make ,T ,@(mapcat (lambda (f) `(,(sym ":" f) (,op (. a ,f) (. b ,f)))) fields))))
                '(+ - *))
         (defop = ((a ,T) (b ,T)) bool
           (and ,@(map (lambda (f) `(= (. a ,f) (. b ,f))) fields))))))

(derive-ops v3)
```

Macros are hygienic. Every symbol written in a template gets a mark unique to
that expansion, so:

- variables the template binds can't capture the user's names, and the user's
  code (passed in as arguments) can't see them: `(swap! tmp other)` works
  with a `swap!` whose template binds its own `tmp`;
- names the template uses without binding mean the global definitions, even if
  the use site has a local of the same name;
- keywords, operators, types, fields and global definitions are unaffected.

To capture a name on purpose (an anaphoric `it`), insert an unmarked symbol
with `,'it`. `gensym` still exists for names a macro builds itself.

## Status

Done: reader, typed core, LLVM code generation and optimisation, C header
import, the C calling convention, bit fields, macros with type queries, and
operator overloading. Next: porting the teapot ray tracer to lisc.
