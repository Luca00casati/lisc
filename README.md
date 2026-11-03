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

(defn main [] i32
  (let [fd (open c"/dev/dri/renderD128" O_RDWR)]
       [id i32 0]
       [gp {i915::drm_i915_getparam :param i915::I915_PARAM_CHIPSET_ID :value (addr id)}]
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
code after a `return`/`break`/`continue`, and the result of a lisc function
dropped (write `(discard (f x))` when that is meant; C functions' results are
often dropped on purpose and aren't warned about). `-w` turns them off.

`:optnone` compiles a function as at `-O0` whatever the level: for code
that runs once (setup, table builders) and is big enough to slow the build.

C imports are cached in `~/.cache/lisc` (or `$XDG_CACHE_HOME/lisc`): each C
module's declarations and constants, keyed by its headers, the flags and the
lisc binary, and checked against every file the headers read (path, size,
modification time). A build with a warm cache does no header parsing.
`--no-cache` imports from the headers every time.

`--time` reports how long each phase took (front end, optimiser, code
generation, link); `-O0` builds fastest.

`--checks` builds with run-time checks that stop the program with the source
location: indexing arrays, slices and strs out of bounds, integer casts that
lose the value (narrowing, or a negative into unsigned), float-to-integer
casts out of range, division by zero. `(truncate T e)` keeps an integer's low
bits on purpose and is never checked.

`-g` adds DWARF debug info: gdb breaks on lisc lines, steps through them and
prints variables (structs, arrays, enums with their member names); code a
macro produced belongs to the line of the macro call. Extra `.c`/`.o` inputs
are linked in. `import-c` searches the source file's
directory and `-I` directories, then the system ones.

## Language

Types: `i8 i16 i32 i64 i128 u8 u16 u32 u64 u128 isize usize f32 f64 f80 bool void str`
(`f80` is C's `long double`, `i128` its `__int128`),
`(ptr T)`, `(array T N)`, `(slice T)`, `(simd T N)`, `(fn (A B ...) R)`, struct/union/enum names, and every
typedef/struct a C module brings in (`libc::size_t`, `libc::FILE` ...).

Top level:

| form | |
|---|---|
| `(import "mod" [:as m])` | a module: its names as `mod::name` (see below); the C modules below are built in |
| `(import-c "h" ... [:as m])` | C headers as a module `m` (by default the first header's name) |
| `(using m ...)` | `m`'s names unqualified, in the whole file — or, inside a function, until the block ends |
| `(include "file.lsp" ...)` | another file's forms as part of this module |
| `(struct name [:packed] [:align N] [field T] ...)` `(union ...)` | C layout; `[field T :bits N]` is a bit field |
| `(enum name T a b (c 5) ...)` | a distinct integer type; members are `name.a` ... |
| `(variant name (case [field T] ...) case ...)` | a tagged union (see Variants) |
| `(export name ...)` | these functions and globals get external linkage and their own name, for C |
| `(const NAME expr)` | compile-time constant; untyped if `expr` is |
| `(def name T [init])` | global variable |
| `(defn name [a T] ... R [:noinline\|:inline\|:cold\|:optnone] body...)` | function (`[]`: no parameters); the last expression is the result |
| `(extern name (T ... [...]) R)` | a C function by hand |
| `(defop op [a T] ... R body...)` | an operator for these operand types |
| `(defmacro name! (params...) body...)` | a macro (see below); its name ends in `!` |
| `(defmeta name (params...) body...)` | a compile-time helper function for macros (belongs to its module, like other names) |
| `(defn- ...)` `(def- ...)` `(const- ...)` `(struct- ...)` `(defmacro- ...)` `(private form ...)` | names only their module can use |

Expressions — everything is one; statements have type `void`:

    (do e...)  (let [x init] [y T init] [z T] body...)  (set place e)
    (if c a [b])  (when c body...)  (unless c body...)
    (while c body...)  (for [i [T] start end [step]] body...)  (break)  (continue)  (return [e])
    (for [x :in xs] body...)  (for [i x :in xs] body...) — each element of an array, simd, slice, str,
      or a struct with data and len (a (vec T)), also through a pointer; x is the element itself
      (setting it sets the element), i its index (a u64)
    (match x (case body...) ((case a b) body...) ... (else body...)) — see Variants
    (print x ...)  (println x ...)  [:to file] — see Printing
    (cond (test body...) ... (else body...))   (case x (v body...) ((v1 v2) body...) ... (else body...))
    (defer e ...) — runs when the enclosing block exits (end, return, break, continue), innermost first
    (slice-of a [from [to]]) — an array as a (slice T), or part of an array, slice or str
    (+ - * / % & | ^ << >>)  (- x)  (~ x)  (= != < <= > >=)  (and ...)  (or ...)  (not x)
    (+= place e) ... (>>= place e) — the place's address is computed once; defops apply
    (atomic-load p) (atomic-store p v) (atomic-swap/add/sub/and/or/xor p v) (atomic-cas p old new) (fence)
      — each with an optional :relaxed :acquire :release :acq-rel :seq-cst (default)
    (. x field ...) — also written x.field.sub; pointers are followed automatically
    a[i] = (aref a i)  (deref p)  (addr place)  (cast T e)  (truncate T e)  (bitcast T e)  (sizeof T)  (alignof T)
    {T :field e ...} = (make T :field e ...)  {(array T N) e ...}  — the rest is zero
    "text" is a str;  c"text" is a C string;  #\a #\é #\newline — a character's code point;  true false
    null — the zero value of the wanted type;  undefined — no value at all
    (defined NAME)  (if-defined NAME then [else]) — compile-time, like #ifdef; -D NAME[=value] defines one

Three kinds of brackets:

- `[ ... ]` is a binding: a parameter `[a T]`, a `let` binding `[x T init]`, a
  `for` header `[i 0 n]`, a struct field `[x f32]`. They stand one after
  another, `(let [x 1] [y 2] body...)`, where the old spelling had one list
  of them, `(let ((x 1) (y 2)) body...)`, which still works. `[]` alone is
  no parameters. A `[...]` anywhere else is an error, not a call.
- `{T :field e ...}` is `(make T :field e ...)`.
- `a[i]`, written right after a name, `a.b`, `(...)` or another index, is
  `(aref a i)`: `v.data[v.len]`, `grid[y][x]`, `(f)[0]`. A `.field` may
  follow an index: `pts[i].x`. In a macro template `,v[i]` is `(aref ,v i)`.

Macro templates keep brackets as written, and compile-time code takes them in
`let` too. `tools/brackets.py file.lsp ...` rewrites code in the old spelling
(keeping comments and layout).

Everything starts as zero: `let` bindings without a value, globals, and the
fields and elements `make` doesn't set. `null` is the zero value of the
type wanted — the null pointer, `0`, `0.0`, `false`, an all-zero struct or
array — so `(set v null)` resets a struct. `undefined` opts out:
`(let [buf (array u8 4096) undefined] ...)` leaves the memory as it is, for
code that fills it before reading.

Strings are UTF-8 (source files must be valid UTF-8; `\u{1F600}` writes a
code point, `\xHH` a raw byte). A string is a `str`: `data` (a `(ptr u8)`) and
`len` (its size in bytes, a `u64`). It is exactly its bytes — no terminator, a
`\0` inside is just a byte — and `s[i]` is a byte.

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
  Structs pass through C's `...` by value too (as C passes them).
- Conditions may be `bool`, integers or pointers.
- Every function uses the C calling convention (x86-64 System V), lisc's own
  included: structs pass and return by value to and from C, and C can call
  lisc functions through function pointers.
- Bit fields (imported or declared) read and write like other fields, and can
  be given in a global's constant `make`; only `(addr ...)` of one is an error.
- Floating point is strict IEEE (no fast-math, no FMA contraction), so results
  match a C build with `-ffp-contract=off`.

## Variants

A variant is a tagged union: a value that is one of its cases, each with its own fields.

```lisp
(variant shape (circle [r f32]) (rect [w f32] [h f32]) none)

(defn area [s shape] f32
  (match s
    ((circle r) (* 3.0 r r))
    ((rect w h) (* w h))
    (none 0.0)))

(area (shape.rect 2.0 3.0))   ; a case with fields is built like a call
(area shape.none)             ; one without is a value
```

`match` runs the clause of the case the value holds; `(case a b ...)` names
the case's fields in order (`_` skips one), and the names are the fields
themselves: `(set r 5.0)` changes the matched value, when that is a place. It
follows pointers. Without an `else`, every case must be there (or it's an
error naming the one missing). A match is a value when its clauses are.

Variants can be generic; their types are found from the values, or from the
type wanted where there are no values to go by:

```lisp
(variant (option T) (some [value T]) none)   ; these two are in std/option
(variant (result T E) (ok [value T]) (err [error E]))

(defn find [xs (slice i32)] [want i32] (option u64)
  (for [i x :in xs] (when (= x want) (return (option.some i))))
  option.none)                                ; (option u64): from the result type
```

A variant is a struct `[tag u32] [cases ...]` (`tag` is the case's index),
with C layout, so it passes to C and through `...` like other structs.

## Printing

`(print x ...)` writes each value as its type shows it, one after another;
`(println x ...)` adds a newline, and `:to f` first writes to the C `FILE` f
instead of stdout:

```lisp
(println "p = " p " n = " n)   ; p = {point :x 1 :y -2} n = 7
(println :to libc::stderr "failed: " (parse s))   ; failed: (result.err "empty")
```

Strings are written as they are (inside a struct, quoted); integers and floats
as printf's `%d` and `%g`; `bool`s as `true`/`false`; enums as `color.blue`;
structs as `{name :field value ...}` (as `make` takes them); variants as
`(shape.circle 2)` or `shape.none`; arrays, slices and simds as `[1 2 3]`;
C strings (`(ptr i8)`) as text, other pointers as addresses. A program's own
function named `print` or `println` comes first. Everything goes through as
few `printf` calls as possible; for formats of your own, `printf` is there.

## SIMD

`(simd T N)` is N integers or floats side by side in a vector register (N a
power of two, up to 64 bytes): C's `__attribute__((vector_size))` types and
`__m128`, which imported headers bring in as simds.

```lisp
(let [a {(simd f32 4) 1.0 2.0 3.0 4.0}] [b (splat (simd f32 4) 0.5)]
  (+ (* a 2.0) b)         ; element by element; a number goes to every element
  a[3]                     ; an element (a place: (set a[3] 8.0))
  (simd-sum (* a b)))      ; a dot product
```

- `+ - * / %`, and `& | ^ << >>` on integers, work element by element, and so
  do `(cast (simd T N) v)` (to another simd of the same length) and `bitcast`.
- `(splat T x)` x in every element; `(shuffle v i ...)` v's elements i ...
  (any power of two of them).
- `(simd-sum v)` `(simd-hmin v)` `(simd-hmax v)` — all elements into one
  (`simd-sum` adds in order, so floats give what a loop would).
- `(simd-min a b)` `(simd-max a b)` `(simd-abs v)` `(simd-sqrt v)` — element by element.
- Simds up to 16 bytes pass in one SSE register as C's vectors do; bigger ones
  in memory, as C passes them without AVX.

## Generics

A struct or function with a list for its name takes type parameters, and is
specialised (compiled anew) for each list of types it is used with:

```lisp
(struct (vec T) [data (ptr T)] [len u64] [cap u64])

(defn (push T) [v (ptr (vec T))] [x T] void ...)
(defn (biggest T) [a T] [b T] T (if (> a b) a b))

(let [v {(vec i32)}]     ; (vec i32) is a type wherever types go
  (push (addr v) 42)             ; T from the arguments' types: push for i32
  ((push i32) (addr v) 42)       ; or written out
  (biggest 2.5 1.0))             ; only literals: their own type (f64)
```

Inside, a parameter is the type itself, not its name: `(sizeof T)`,
`(cast (ptr T) p)`, `(let [s T null] ...)` and macros' `type-of` all see the
instance's types, whichever module they come from. An error in an instance
says which one and where it was used. A generic that is never used is never
compiled, so its body is only checked once something uses it.

A parameter can say which operators its type must have, `(T op ...)`; using
the generic with a type that lacks one is an error at the use, naming it:

```lisp
(defn (sort (T <)) [s (slice T)] void ...)    ; numbers, or a struct with a (defop < ...)
```

A generic function is a value too: `(addr (inc i64))`, or just `inc` where a
function pointer is wanted (its type says what T is): `(apply-twice inc 2.5)`.

Operators can be generic, for every instance of a generic struct:

```lisp
(defop (+ T) [a (vec2 T)] [b (vec2 T)] (vec2 T) (v2 (+ a.x b.x) (+ a.y b.y)))
```

A generic `defop` applies only when an operand is a struct, so numbers keep
their own operators.

## Modules

Every file is a module, named after the file. `(import "scene")` (path
relative to the importing file, `.lsp` optional) makes its names reachable as
`scene::build`; there is one level only (`a::b::c` is an error).

    (import "scene")             ; scene::build, scene::camera ...
    (import "rt-kernel" :as rt)  ; rt::build
    (import "vec")
    (using vec)                  ; also dot, cross ... unqualified

    (defn hyp [a f32] [b f32] f32
      (using math)               ; just here: sqrtf
      (sqrtf (+ (* a a) (* b b))))

A name resolves to the module's own definition first, then to the modules in
use (the innermost block's, then the file's; a name two of them define must
be written qualified), then to `-D` constants. Definitions marked
private (`defn-`, `(private ...)` ...) are unreachable from other modules.

Macros are hygienic across modules too: the symbols of a macro's template
resolve in the macro's own module, so `(geom::doubled! v)` works anywhere
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

## Standard library

`(import "x")` looks next to the importing file, then in `-I` directories, then
in `lib/` next to the `lisc` binary, which holds the standard library:

| module | |
|---|---|
| `std/str` | `eq` `starts-with` `ends-with` `find` on `str`; `from-c` / `to-c` (C strings); `print` `fprint` |
| `std/mem` | `(mem::new! T n)` — n zeroed T's; `free` |
| `std/vec` | `(vec T)` — a growable array; `push` `pop` `clear` `items` (a slice) `release` |
| `std/sort` | `(sort s)` — a slice in place, by `<`; `(sort-by s less)` with a `(ptr (fn (T T) bool))` |
| `std/option` | `(option T)`: `(option.some x)` or `option.none`; `(result T E)`: `(result.ok x)` or `(result.err e)`; `or-else` `is-some` `is-ok` |
| `std/map` | `(map K V)` — a hash map; `put` `get` (a pointer, or null) `has` `remove` `release`, `(for-each! (k v m) ...)`; keys by value, strs by contents, structs by bytes |

## Operators

`(defop + [a v3] [b v3] v3 ...)` defines `+` for two `v3`s. An operator form
uses the definition whose parameter types match its operands exactly; a
literal operand takes the type the definition wants (`(* 2.0 v)` with
`(defop * [k f32] [a v3] ...)`). n-ary forms fold left: `(+ a b c)` is
`(+ (+ a b) c)`. Unary `-` and the comparisons can be defined the same way.
Definitions are ordinary functions, so the optimiser inlines them: there is no
runtime cost.

## Macros

A macro's name ends in `!`, and only a macro's does, so a call that is a
macro is plain to see: `(swap! a b)`, `(mem::new! f32 4)`. (A call written
without the `!` gets told so; `defmeta` helpers are functions, not macros,
and have no `!`.)

A macro is a function that runs inside the compiler, on forms: it receives its
arguments unevaluated and returns the form to compile in its place. The
language inside is a small Lisp (see `src/macro.h` for the full list):
quasiquote, `if`/`cond`/`let`/`while`/`lambda`, list functions (`map`,
`mapcat`, `filter`, `range`, ...), `sym`/`str`/`gensym`, and parameters that
destructure (`(defmacro unroll! ((i n) &body body) ...)`).

Macros can ask about types: `(type-of e)` is the type `e` has where the macro
is used, and `(type-kind T)`, `(type-elem T)`, `(type-fields T)` ... describe a
type. So a macro can dispatch on its argument's type, or generate code for every
field of a struct:

```lisp
(defmacro derive-ops! (T)                 ; + - * and = for any struct, field by field
  (let [fields (map first (type-fields T))]
    `(do ,@(map (lambda (op)
                  `(defop ,op [a ,T] [b ,T] ,T
                     {,T ,@(mapcat (lambda (f) `(,(sym ":" f) (,op (. a ,f) (. b ,f)))) fields)}))
                '(+ - *))
         (defop = [a ,T] [b ,T] bool
           (and ,@(map (lambda (f) `(= (. a ,f) (. b ,f))) fields))))))

(derive-ops! v3)
```

`type-of` works in top-level macros too, for types, literals and C
declarations (`(type-of (libc::strlen c""))` is `u64`); the program's own
functions, globals and constants are declared after top-level macros run, so
it can't see those there.

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
operator overloading, generics, variants with match, typed printing, SIMD types. The teapot ray tracer is written in lisc.
