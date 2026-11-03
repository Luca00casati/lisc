// abi.h — C functions taking and returning structs by value, for tests/abi.lsp
#include <stdbool.h>

typedef struct { float x, y, z; } v3;            // SSE, SSE: <2 x float>, float
typedef struct { double a, b; } d2;              // SSE, SSE
typedef struct { int i; float f; } mix;          // one INTEGER eightbyte
typedef struct { float f; int i; double d; } fi; // INTEGER, SSE
typedef struct { char c; short s; } sm;          // i32
typedef struct { long a, b; } l2;                // INTEGER, INTEGER
typedef struct { int v[5]; } big;                // 20 bytes: memory
typedef struct { float v[3]; } f3;               // an array inside: like v3
struct bf { unsigned a : 3; unsigned b : 5; int c : 7; unsigned d : 17; bool e : 1; signed char tail; };

v3 v3_scale(v3 v, float k);
double d2_sum(d2 d);
d2 d2_swap(d2 d);
mix mix_make(int i, float f);
fi fi_twice(fi x);
sm sm_inc(sm x);
l2 l2_add(l2 a, l2 b);
big big_rev(big b);
f3 f3_neg(f3 v);
long many_ints(long a, long b, long c, long d, long e, long f, l2 s);           // s no longer fits: stack
double many_doubles(double a, double b, double c, double d, double e, double f, double g, d2 s, double after);
long partial(long a, long b, long c, long d, long e, l2 s, long g);             // s needs 2, 1 left: stack; g in a register
short small(unsigned char u, signed char s);                                     // zero/sign extension
double apply_v3(float (*f)(v3), v3 v);                                          // C calls lisc
big apply_big(big (*f)(big), big b);
void bf_set(struct bf *p);
long bf_sum(struct bf b);
// variadic: a format letter per argument (v v3, d d2, m mix, l l2, b big, i int)
double var_structs(const char *fmt, ...);
// long double (f80) and __int128 (i128/u128)
long double ld_mul(long double a, double b);
__int128 i128_mul(long a, long b);
unsigned __int128 u128_shift(unsigned __int128 x, int n);
typedef struct { __int128 v; long tag; } wide;     // INTEGER, INTEGER, then memory (24 bytes)
typedef struct { __int128 v; } w1;                 // INTEGER, INTEGER: two registers
typedef struct { long double x; int n; } ldn;      // X87 inside: memory
w1 w1_twice(w1 a);
long double ldn_sum(ldn a, ldn b);
long wide_low(wide w);
typedef float v4f __attribute__((vector_size(16)));   // one SSE register
typedef int v8i __attribute__((vector_size(32)));     // memory (no AVX)
v4f v4f_mad(v4f a, v4f b, float k);
int v8i_sum(v8i a, int extra, v8i b);
