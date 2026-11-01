#include "abi.h"

v3 v3_scale(v3 v, float k) { return (v3){v.x * k, v.y * k, v.z * k}; }
double d2_sum(d2 d) { return d.a + d.b; }
d2 d2_swap(d2 d) { return (d2){d.b, d.a}; }
mix mix_make(int i, float f) { return (mix){i, f}; }
fi fi_twice(fi x) { return (fi){x.f * 2, x.i * 2, x.d * 2}; }
sm sm_inc(sm x) { return (sm){x.c + 1, x.s + 1}; }
l2 l2_add(l2 a, l2 b) { return (l2){a.a + b.a, a.b + b.b}; }
big big_rev(big b) { big r; for (int i = 0; i < 5; i++) r.v[i] = b.v[4 - i]; return r; }
f3 f3_neg(f3 v) { return (f3){{-v.v[0], -v.v[1], -v.v[2]}}; }
long many_ints(long a, long b, long c, long d, long e, long f, l2 s) { return a + b + c + d + e + f + 100 * s.a + 1000 * s.b; }
double many_doubles(double a, double b, double c, double d, double e, double f, double g, d2 s, double after) {
    return a + b + c + d + e + f + g + 100 * s.a + 1000 * s.b + 10000 * after;
}
long partial(long a, long b, long c, long d, long e, l2 s, long g) { return a + b + c + d + e + 100 * s.a + 1000 * s.b + 10000 * g; }
short small(unsigned char u, signed char s) { return u + s; }
double apply_v3(float (*f)(v3), v3 v) { return f(v) * 2; }
big apply_big(big (*f)(big), big b) { big r = f(b); r.v[0] += 1000; return r; }
void bf_set(struct bf *p) { p->a = 5; p->b = 17; p->c = -33; p->d = 100000; p->e = 1; p->tail = -2; }
long bf_sum(struct bf b) { return b.a + 10L * b.b + 100L * b.c + 1000L * b.d + 1000000000L * b.e + 7 * b.tail; }
