# lisc

A compiled language with Lisp syntax and C's types. Programs are
s-expressions; the compiler (C++, LLVM 18) type-checks them and emits
optimised native code linked against libc. C headers are read through
libclang into modules (`libc::printf`, `posix::open`, `math::sinf`, or any
header with `import-c`), so libc and kernel interfaces need no hand-written bindings.

```lisp
(import "libc") (import "posix") (import "io")
(import-c "drm/i915_drm.h" :as i915)
(using libc posix io)

(defn main () i32
  (let ((fd (open c"/dev/dri/renderD128" O_RDWR))
        (id i32 0)
        (gp (make i915::drm_i915_getparam :param i915::I915_PARAM_CHIPSET_ID :value (addr id))))
    (when (= (ioctl fd i915::DRM_IOCTL_I915_GETPARAM (addr gp)) 0)
      (printf c"i915 chip id 0x%04x\n" id)))
  0)
```

## Build

    make            # needs llvm-18 and libclang-18 (headers + libraries)
    make test
    ./lisc prog.lsp [more.c|.o ...] [-o prog] [-O0..-O3] [-I dir] [-D NAME[=value]] [--emit-llvm] [-c] [-lname ...]

Errors in function bodies are all reported (up to 20), not just the first.
Warnings: a variable that is never read (name it `_x` to say that is meant),
and code after a `return`/`break`/`continue`; `-w` turns them off.

`-g` adds DWARF debug info: gdb breaks on lisc lines, steps through them and
prints variables (structs, arrays, enums with their member names); code a
macro produced belongs to the line of the macro call. Extra `.c`/`.o` inputs
are linked in. `import-c` searches the source file's
directory and `-I` directories, then the system ones.

## Language

Types: `i8 i16 i32 i64 u8 u16 u32 u64 isize usize f32 f64 bool void str`,
`(ptr T)`, `(array T N)`, `(slice T)`, `(fn (A B ...) R)`, struct/union/enum names, and every
typedef/struct a C module brings in (`libc::size_t`, `libc::FILE` ...).

Top level:

| form | |
|---|---|
| `(import "mod" [:as m])` | a module: its names as `mod::name` (see below); the C modules below are built in |
| `(import-c "h" ... [:as m])` | C headers as a module `m` (by default the first header's name) |
| `(using m ...)` | `m`'s names unqualified, in the whole file — or, inside a function, until the block ends |
| `(include "file.lsp" ...)` | another file's forms as part of this module |
| `(struct name [:packed] [:align N] (field T) ...)` `(union ...)` | C layout; `(field T :bits N)` is a bit field |
| `(enum name T a b (c 5) ...)` | a distinct integer type; members are `name.a` ... |
| `(export name ...)` | these functions and globals get external linkage and their own name, for C |
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
    (cond (test body...) ... (else body...))   (case x (v body...) ((v1 v2) body...) ... (else body...))
    (defer e ...) — runs when the enclosing block exits (end, return, break, continue), innermost first
    (slice-of a [from [to]]) — an array as a (slice T), or part of an array, slice or str
    (+ - * / % & | ^ << >>)  (- x)  (~ x)  (= != < <= > >=)  (and ...)  (or ...)  (not x)
    (+= place e) ... (>>= place e) — the place's address is computed once; defops apply
    (atomic-load p) (atomic-store p v) (atomic-swap/add/sub/and/or/xor p v) (atomic-cas p old new) (fence)
      — each with an optional :relaxed :acquire :release :acq-rel :seq-cst (default)
    (. x field ...) — also written x.field.sub; pointers are followed automatically
    (aref a i)  (deref p)  (addr place)  (cast T e)  (bitcast T e)  (sizeof T)  (alignof T)
    (make T :field e ...)  (make (array T N) e ...)  — the rest is zero
    "text" is a str;  c"text" is a C string;  #\a #\é #\newline — a character's code point;  true false
    null — the zero value of the wanted type;  undefined — no value at all
    (defined NAME)  (if-defined NAME then [else]) — compile-time, like #ifdef; -D NAME[=value] defines one

Everything starts as zero: `let` bindings without a value, globals, and the
fields and elements `make` doesn't set. `null` is the zero value of the
type wanted — the null pointer, `0`, `0.0`, `false`, an all-zero struct or
array — so `(set v null)` resets a struct. `undefined` opts out:
`(let ((buf (array u8 4096) undefined)) ...)` leaves the memory as it is, for
code that fills it before reading.

Strings are UTF-8 (source files must be valid UTF-8; `\u{1F600}` writes a
code point, `\xHH` a raw byte). A string is a `str`: `data` (a `(ptr u8)`) and
`len` (its size in bytes, a `u64`). It is exactly its bytes — no terminator, a
`\0` inside is just a byte — and `(aref s i)` is a byte.

C strings are written `c"..."`: the bytes and a terminating zero, a `(ptr i8)`
(or a `(ptr u8)` / `(ptr void)` where one is wanted). That is the only kind of
literal C functions take, so the terminator is always visible in the source:

    (printf c"%d items\n" n)
    (printf c"%.*s\n" (cast i32 s.len) s.data)   ; a str, given to C with its size
    (fopen "out.ppm" c"wb")                      ; error: a str is not a C string

`-D NAME=text` values are C strings (they are for C, like C macros); in macros,
`(cstr ...)` builds a `c"..."` literal as `(str ...)` builds a plain one.

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
    (import "vec")
    (using vec)                  ; also dot, cross ... unqualified

    (defn hyp ((a f32) (b f32)) f32
      (using math)               ; just here: sqrtf
      (sqrtf (+ (* a a) (* b b))))

A name resolves to the module's own definition first, then to the modules in
use (the innermost block's, then the file's; a name two of them define must
be written qualified), then to `-D` constants. Definitions marked
private (`defn-`, `(private ...)` ...) are unreachable from other modules.

Macros are hygienic across modules too: the symbols of a macro's template
resolve in the macro's own module, so `(geom::doubled v)` works anywhere
even though its expansion calls geom's private helpers. Operators (`defop`)
are chosen by type, so they apply wherever their types are used.

C modules hold a header's functions, extern variables, structs (with
clang's exact layout, bit fields included), typedefs, enum constants and
constant `#define`s. The built-in ones:

| module | headers |
|---|---|
| `libc` | stdio, stdlib, string, ctype, errno, limits, stdint, stdbool |
| `math` | math |
| `posix` | unistd, fcntl, sys/stat, sys/types |
| `io` | poll, sys/ioctl, sys/mman |
| `signal` | signal |
| `time` | time |
| `thread` | pthread |

C names keep their C symbols, so `libc::printf` links to `printf`. C headers
include each other, so two C modules can declare the same thing (`size_t`,
`NULL`); using both is fine, it is one name.

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

Done: reader, typed core, LLVM code generation and optimisation, modules
(with C headers as modules), the C calling convention, bit fields, macros with type queries, and
operator overloading. Next: porting the teapot ray tracer to lisc.
