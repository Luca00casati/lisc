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

(defn main [] -> i32
  (let [fd (open c"/dev/dri/renderD128" O_RDWR)]
       [id i32 0]
       [gp {i915::drm_i915_getparam :param i915::I915_PARAM_CHIPSET_ID :value (addr id)}]
    (when (= (ioctl fd i915::DRM_IOCTL_I915_GETPARAM (addr gp)) 0)
      (printf c"i915 chip id 0x%04x\n" id)))
  (return 0))
```

## Build

    make            # needs llvm-18 and libclang-18 (headers + libraries)
    make test
    ./lisc prog.lsp [more.c|.o ...] [-o prog] [-O0..-O3] [-I dir] [-D NAME[=value]] [--emit-llvm] [-c] [-lname ...]
    ./lisc fmt [--check] file.lsp ...   # the canonical layout (see below)

Errors in function bodies are all reported (up to 20), not just the first.
Warnings: a variable that is never read (name it `_x` to say that is meant),
code after a `return`/`break`/`continue`, and the result of a lisc function
dropped (write `(discard (f x))` when that is meant; C functions' results are
often dropped on purpose and aren't warned about). `-w` turns them off.

Builds are parallel and incremental. The program is compiled in parts, one
per module, each optimised and compiled to machine code on its own thread;
small functions a part calls in other modules are copied into it for
inlining, so splitting costs no speed. Each part's object is cached in
`~/.cache/lisc/obj` under a hash of its exact code and the build's settings,
so a rebuild compiles only the parts whose code changed (an edit to a small
function also recompiles the parts that inline it); a comment or a change
elsewhere costs nothing. The teapot ray tracer (~4000 lines, 14 parts, 4
cores) builds at `-O2` in 0.9 s from scratch, 0.2 s with nothing changed,
and 0.3–0.45 s after editing a module. `--no-cache` turns both caches off;
objects unused for a month are removed. `-c` and `--emit-llvm` produce one
file from one module.

`:optnone` compiles a function as at `-O0` whatever the level: for code
that runs once (setup, table builders) and is big enough to slow the build.
A module's optnone functions are a part of their own, compiled with the code
generator's fast path.

`lisc fmt` lays files out the canonical way without moving line breaks: it
sets each line's indentation (a body form's body 2 in; a call's arguments
under its first one; a `let`'s or `defn`'s further `[bindings]` under the
first; a comment line as the code after it), one space between tokens (none
inside brackets; tokens written touching, like `a[i]` and `'x`, stay so),
trailing comments at their column, no trailing whitespace, at most one blank
line in a row. `--check` changes nothing and lists the files it would change.

C imports are cached in `~/.cache/lisc` (or `$XDG_CACHE_HOME/lisc`): each C
module's declarations and constants, keyed by its headers, the flags and the
lisc binary, and checked against every file the headers read (path, size,
modification time). A build with a warm cache does no header parsing.
`--no-cache` imports from the headers every time.

`--time` reports how long each phase took (front end, compiling the parts,
link) and how many parts came from the cache; `-O0` builds fastest.

Every build checks, at run time, indexing, integer division and number
conversions, and stops the program with the source location
(`file:line:col: index 5 out of bounds (length 5)`):

- an index into an array, a simd, or anything with `data` and `len` fields (a
  str, a slice, a `(vec T)`: `v[i]`); a constant index past an array's end is
  a compile-time error. A raw pointer's `p[i]` has no length and isn't checked:
  prefer slices.
- `(slice-of a from to)`: `from <= to <= length`
- integer `/` and `%`: by zero, and the minimum divided by -1
- a null pointer followed: `(deref p)`, `p[i]`, `p.field` through a pointer, a
  call through a function pointer, and a null closure or heap-fn called
- a heap-fn called after `drop`, or dropped twice. drop marks its env dead and
  keeps it in a quarantine of the last 256 drops before freeing it, so this is
  caught within those (as AddressSanitizer does), not after
- `(cast T e)` of an integer that loses the value: narrowed, or a negative
  into an unsigned type. A literal cast is read as that type, and one that
  doesn't fit is a compile-time error. `(truncate T e)` keeps an integer's low
  bits on purpose and is never checked.

A float cast to an integer doesn't stop: it saturates, as in Rust. Out of
range gives the type's minimum or maximum, NaN gives 0: `(cast u8 300.5)` is
255, `(cast i32 -1e10)` is the smallest i32.

Bounds checks in loops mostly cost nothing after optimisation (LLVM proves
them); saturating float casts cost the most (a few instructions each). The
teapot's CPU side is about 1.5% slower with all of them on. To leave them out:
`(unchecked body...)` for a block, `:unchecked` after a function's result type
for a whole function (`(defn sum [xs (slice i32)] -> i32 :unchecked ...)`), or
`--no-checks` for the whole build. There, float casts are C's: a value out of
range gives an undefined result. `(cast-force T e)` is that plain conversion
for one cast (no check, no saturation; an integer keeps its low bits), for hot
code whose values are known to fit; its operand is still checked.

A failed check flushes what the program printed before it, then prints the
error to stderr and aborts.

`-g` adds DWARF debug info: gdb breaks on lisc lines, steps through them and
prints variables (structs, arrays, enums with their member names). Extra `.c`/`.o` inputs
are linked in. `import-c` searches the source file's
directory and `-I` directories, then the system ones.

## Language

Types: `i8 i16 i32 i64 i128 u8 u16 u32 u64 u128 isize usize f32 f64 f80 bool void str`
(`f80` is C's `long double`, `i128` its `__int128`),
`(ptr T)`, `(array T N)`, `(slice T)`, `(simd T N)`, `(fn (A B ...) R)`, `(closure (A B ...) R)`, `(heap-fn (A B ...) R)`, struct/union/enum names, and every
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
| `(defn name [a T] ... [-> R] [:noinline\|:inline\|:cold\|:optnone\|:unchecked] body...)` | function (`[]`: no parameters); with `-> R` it gives an R, only by `(return e)` (see Functions) |
| `(extern name (T ... [...]) R)` | a C function by hand |
| `(defop op [a T] ... -> R body...)` | an operator for these operand types |
| `(private form ...)` | definitions (any kind) only their module can use |

Expressions — everything is one; statements have type `void`:

    (do e...)  (let [x init] [y T init] [z T] body...)  (set place e)
    (if c a [b])  (when c body...)  (unless c body...)
    (while c body...)  (for [i [T] start end [step]] body...)  (break)  (continue)  (return [e])
    (for [x :in xs] body...)  (for [i x :in xs] body...) — each element of an array, simd, slice, str,
      or a struct with data and len (a (vec T)), also through a pointer; x is the element itself
      (setting it sets the element), i its index (a u64)
    (match x (case body...) ((case a b) body...) ((or c d) body...) ... (else body...)) — see Variants
    (lambda [a T] ... [-> R] body...)  (lambda [a T] ... -> e) — a function written where it is used; with captures a closure
    (lambda :heap [a T] ... [-> R] body...) — a heap-fn, kept until (drop f)
    (unchecked body...) — no run-time checks in it
    (print x ...)  (println x ...)  [:to file] — see Printing
    (cond (test body...) ... (else body...))   (case x (v body...) ((v1 v2) body...) ... (else body...))
    (defer e ...) — runs when the enclosing block exits (end, return, break, continue), innermost first
    (slice-of a [from [to]]) — an array as a (slice T), or part of an array, slice or str
    (+ - * / % & | ^ << >>)  (- x)  (~ x)  (= != < <= > >=)  (and ...)  (or ...)  (not x)
    (+= place e) ... (>>= place e) — the place's address is computed once; defops apply
    (atomic-load p) (atomic-store p v) (atomic-swap/add/sub/and/or/xor p v) (atomic-cas p old new) (fence)
      — each with an optional :relaxed :acquire :release :acq-rel :seq-cst (default)
    (. x field ...) — also written x.field.sub; pointers are followed automatically
    a[i] = (aref a i)  (deref p)  (addr place)  (cast T e)  (cast-force T e)  (truncate T e)  (bitcast T e)  (sizeof T)  (alignof T)
    {T :field e ...} = (make T :field e ...)  {(array T N) e ...}  — the rest is zero
    "text" is a str;  c"text" is a C string;  #\a #\é #\newline — a character's code point;  true false
    null — the zero value of the wanted type;  undefined — no value at all
    (defined NAME)  (if-defined NAME then [else]) — compile-time, like #ifdef; -D NAME[=value] defines one;
      at the top level it chooses imports and definitions ((do ...) for several), by -D names only

Three kinds of brackets:

- `[ ... ]` is a binding: a parameter `[a T]`, a `let` binding `[x T init]`, a
  `for` header `[i 0 n]`, a struct field `[x f32]`. They stand one after
  another, `(let [x 1] [y 2] body...)`. `[]` alone is no parameters. This is
  the only spelling: the old one, a list of them in parentheses
  (`(let ((x 1) (y 2)) ...)`, `(defn f () ...)`), is an error. A `[...]`
  anywhere else is an error too, not a call.
- `{T :field e ...}` is `(make T :field e ...)`.
- `a[i]`, written right after a name, `a.b`, `(...)` or another index, is
  `(aref a i)`: `v.data[v.len]`, `grid[y][x]`, `(f)[0]`. A `.field` may
  follow an index: `pts[i].x`.

`tools/brackets.py file.lsp ...` rewrites code in the old spelling
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

`-D NAME=text` values are C strings (they are for C, like C macros).

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

## Functions

`(defn name [a T] [b U] -> R body...)` gives an R; the result type comes after
`->`. A function gives its result only with `(return e)`: the body is
statements, and a function with a result that can reach its end without
returning is an error. Without `-> R` a function gives nothing (`-> void` is
allowed).

```lisp
(defn sign [x i32] -> i32
  (if (< x 0) (return -1) (return 1)))     ; every path returns

(defn first-big [xs (slice i32)] -> i32
  (for [x :in xs]
    (when (> x 10) (return x)))
  (return 0))

(defn greet [name str]                     ; no result
  (println "hello " name))
```

Blocks are still expressions inside a function: `(set x (if c 1 2))`,
`(let [y (let [t 2] (* t t))] ...)`. `(while true ...)` is left only by
`break` or `return`, so a function ending in one needs nothing after it.
`tools/returns.py file.lsp ...` rewrites code from the older style (a result
type right after the parameters, the last expression the result).

Functions can be written where they are used, with `lambda`:
`(lambda [a T] ... -> R body...)` as a `defn`, or, for one expression,
`(lambda [a T] ... -> e)`: a single form after `->` is the result itself (a
result type would need a body), its type the one wanted (a function pointer,
closure or heap-fn type) or else the expression's. A lambda that uses only its
parameters, globals and other functions is a function pointer, typed
`(ptr (fn (T ...) R))`, and can go to C:

```lisp
(sort-by (slice-of xs) (lambda [a i32] [b i32] -> (> a b)))   ; descending
(let [inc (lambda [x i32] -> (+ x 1))] (inc 41))
(lambda [x i32] -> i32                                          ; the long form
  (let [y (* x 3)]
    (return (+ y 1))))
```

One that uses locals around it is a **closure**, typed `(closure (T ...) R)`.
It captures them by reference: it reads and sets the variables themselves.

```lisp
(defn (each T) [xs (slice T)] [f (closure (T) void)]
  (for [x :in xs] (f x)))

(let [total 0] [k 3]
  (each (slice-of a) (lambda [x i32] (+= total (* x k))))
  total)
```

A closure is two pointers: the function and its env (where the captures
are, in the frame of the function that made it). So a closure only goes
down: it is an argument, a parameter, or a local given one when it is made.
It can't be set again, returned, kept in a global, a field, an array or a
slice, or put behind a pointer (each is an error). Nothing then outlives the
captured variables, and nothing keeps a closure from an earlier pass of a
loop. A plain function, or a lambda without captures, goes wherever a closure is
wanted. A generic parameter `[f F]` takes either, with no indirection added
(`sort-by` does this).

To keep a function with what it captured (a struct of event handlers, a
`vec` of callbacks, a function that makes one), write `(lambda :heap ...)`: a
**heap-fn**, typed `(heap-fn (T ...) R)`. It captures by value: its captures
are copied into an env `malloc`'d when it is made, and are its own from then
on (setting one changes the copy, kept between calls). It goes anywhere a
value goes, and lives until `(drop f)` frees its env:

```lisp
(struct button [name str] [on-click (heap-fn (i32) void)])

(defn make-greet [who str] [times (ptr i32)] -> (heap-fn (i32) void)
  (return (lambda :heap [x i32]
    (+= (deref times) 1)               ; shared state: capture a pointer to it
    (println "hello " who " at " x))))

(let [clicks 0] [ok {button :name "ok" :on-click (make-greet "ann" (addr clicks))}]
  (ok.on-click 10)
  (drop ok.on-click))
```

As with `malloc`, it is yours to drop once: not dropping it leaks its env,
and calling it (or a copy of it) after the drop reads freed memory (a check
catches that, and a second drop, for the last 256 drops). A plain
function goes where a heap-fn is wanted (dropping it frees nothing), and a
heap-fn goes where a closure is wanted (to `sort-by`, say). A heap-fn can't
capture a closure, which would outlive what that captured.

A lambda is compiled as a function of its own; in a generic, it sees the
instance's types. `(fn (T) R)` is the function type; `fn` is only that.

## Variants

A variant is a tagged union: a value that is one of its cases, each with its own fields.

```lisp
(variant shape (circle [r f32]) (rect [w f32] [h f32]) none)

(defn area [s shape] -> f32
  (match s
    ((circle r) (return (* 3.0 r r)))
    ((rect w h) (return (* w h)))
    (none (return 0.0))))

(area (shape.rect 2.0 3.0))   ; a case with fields is built like a call
(area shape.none)             ; one without is a value
```

`match` runs the clause of the case the value holds; `(case a b ...)` names
the case's fields in order (`_` skips one), and the names are the fields
themselves: `(set r 5.0)` changes the matched value, when that is a place;
`((or circle rect) body...)` is any of several cases (without fields). It
follows pointers. Without an `else`, every case must be there (or it's an
error naming the one missing). A match is a value when its clauses are.

Variants can be generic; their types are found from the values, or from the
type wanted where there are no values to go by:

```lisp
(variant (option T) (some [value T]) none)   ; these two are in std/option
(variant (result T E) (ok [value T]) (err [error E]))

(defn find [xs (slice i32)] [want i32] -> (option u64)
  (for [i x :in xs] (when (= x want) (return (option.some i))))
  (return option.none))                                ; (option u64): from the result type
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
as printf's `%d` and `%g` (`i128`s too); `bool`s as `true`/`false`; enums as `color.blue`;
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
- Comparisons give a mask, as C's do: a simd of signed integers as wide as
  the elements, each -1 (true) or 0. `(simd-select m a b)` takes a's element
  where m's is nonzero, else b's (numbers go to every element);
  `(simd-any m)` and `(simd-all m)` are bools; `& | ^ ~` combine masks.
- Simds up to 16 bytes pass in one SSE register as C's vectors do; bigger ones
  in memory, as C passes them without AVX.

## Generics

A struct or function with a list for its name takes type parameters, and is
specialised (compiled anew) for each list of types it is used with:

```lisp
(struct (vec T) [data (ptr T)] [len u64] [cap u64])

(defn (push T) [v (ptr (vec T))] [x T] ...)
(defn (biggest T) [a T] [b T] -> T (if (> a b) (return a) (return b)))

(let [v {(vec i32)}]     ; (vec i32) is a type wherever types go
  (push (addr v) 42)             ; T from the arguments' types: push for i32
  ((push i32) (addr v) 42)       ; or written out
  (biggest 2.5 1.0))             ; only literals: their own type (f64)
```

When the arguments can't tell T, the types go first in the call:

```lisp
(defn (new T) [n u64] -> (ptr T) (return ...))   ; std/mem
(mem::new f32 4)                     ; T = f32, n = 4
```

That is, a call that starts with as many types as the function has type
parameters, followed by its arguments, gives T outright.

Inside, a parameter is the type itself, not its name: `(sizeof T)`,
`(cast (ptr T) p)`, `(let [s T null] ...)` all see the
instance's types, whichever module they come from. An error in an instance
says which one and where it was used. A generic that is never used is never
compiled, so its body is only checked once something uses it.

A parameter can say which operators its type must have, `(T op ...)`; using
the generic with a type that lacks one is an error at the use, naming it:

```lisp
(defn (sort (T <)) [s (slice T)] ...)    ; numbers, or a struct with a (defop < ...)
```

A generic function is a value too: `(addr (inc i64))`, or just `inc` where a
function pointer is wanted (its type says what T is): `(apply-twice inc 2.5)`.

Operators can be generic, for every instance of a generic struct:

```lisp
(defop (+ T) [a (vec2 T)] [b (vec2 T)] -> (vec2 T) (return (v2 (+ a.x b.x) (+ a.y b.y))))
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

    (defn hyp [a f32] [b f32] -> f32
      (using math)               ; just here: sqrtf
      (return (sqrtf (+ (* a a) (* b b)))))

A name resolves to the module's own definition first, then to the modules in
use (the innermost block's, then the file's; a name two of them define must
be written qualified), then to `-D` constants. Definitions inside
`(private ...)` are unreachable from other modules:

```lisp
(defn area [s shape] -> f32 (return (* (side s) (side s))))   ; public

(private
  (def calls i32 0)
  (defn side [s shape] -> f32 (return ...)))
```

Operators (`defop`) are chosen by type, so they apply wherever their types
are used.

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
| `std/mem` | `(mem::new T n)` — n zeroed T's; `free` |
| `std/vec` | `(vec T)` — a growable array; `push` `pop` `clear` `items` (a slice) `release` |
| `std/sort` | `(sort s)` — a slice in place, by `<`; `(sort-by s less)` with a function or closure `(T T) → bool` |
| `std/option` | `(option T)`: `(option.some x)` or `option.none`; `(result T E)`: `(result.ok x)` or `(result.err e)`; `or-else` `is-some` `is-ok` |
| `std/map` | `(map K V)` — a hash map; `put` `get` (a pointer, or null) `has` `remove` `release`, `(each m f)` (f a function or closure of k and v); strs by contents, other keys by bytes |

## Operators

`(defop + [a v3] [b v3] -> v3 ...)` defines `+` for two `v3`s. An operator form
uses the definition whose parameter types match its operands exactly; a
literal operand takes the type the definition wants (`(* 2.0 v)` with
`(defop * [k f32] [a v3] -> v3 ...)`). n-ary forms fold left: `(+ a b c)` is
`(+ (+ a b) c)`. Unary `-` and the comparisons can be defined the same way.
Definitions are ordinary functions, so the optimiser inlines them: there is no
runtime cost.

`defop` also overloads a name: a function with several definitions, chosen by
the types of the arguments (and their number), in its module like other names:

```lisp
(defop min [a f32] [b f32] -> f32 (if (< a b) (return a) (return b)))
(defop min [a v3] [b v3] -> v3 (return {v3 :x (min a.x b.x) :y (min a.y b.y) :z (min a.z b.z)}))
(defop show [s str] ...)
(defop (twice T) [x T] -> T (return (+ x x)))     ; generic ones take any types
```

A name is either a `defn` or a `defop` in a module, not both. Where another
module's plain function has the same name (`math::pow` and a `defop pow`),
arguments no definition fits go to that function.

## Status

Done: reader, typed core, LLVM code generation and optimisation, modules
(with C headers as modules), the C calling convention, bit fields, and
operator overloading, generics, variants with match, typed printing, SIMD types. The teapot ray tracer is written in lisc.
