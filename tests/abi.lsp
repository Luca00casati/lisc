; structs by value across the C calling convention, both ways, and bit fields
(import "libc")
(using libc)
(import-c "abi.h" :as cabi)
(import "math")
(using cabi)

; a lisc struct laid out like C's struct bf, bit fields included
(struct mybf [a u32 :bits 3] [b u32 :bits 5] [c i32 :bits 7] [d u32 :bits 17] [e bool :bits 1] [tail i8])

; bit fields in a constant initializer: the same bytes as C's
(def gbf mybf {mybf :a 5 :b 17 :c -33 :d 100000 :e true :tail -2})
(def gcbf bf {bf :a 5 :b 17 :c -33 :d 100000 :e true :tail -2})

(defn len2 [v v3] f32 (+ (* v.x v.x) (* v.y v.y) (* v.z v.z)))   ; called from C
(defn rot [b big] big                                              ; called from C, big result
  {big :v {(array i32 5) b.v[1] b.v[2] b.v[3] b.v[4] b.v[0]}})

(defn main [] i32
  (let [v (v3_scale {v3 :x 1.0 :y 2.0 :z 3.0} 0.5)]
    (printf c"v3 %g %g %g\n" (cast f64 v.x) (cast f64 v.y) (cast f64 v.z)))
  (let [d (d2_swap {d2 :a 1.5 :b 2.25})]
    (printf c"d2 %g %g sum %g\n" d.a d.b (d2_sum d)))
  (let [m (mix_make -7 0.25)] [x (fi_twice {fi :f 1.5 :i 21 :d 0.125})]
    (printf c"mix %d %g  fi %g %d %g\n" m.i (cast f64 m.f) (cast f64 x.f) x.i x.d))
  (let [s (sm_inc {sm :c 9 :s -300})] [l (l2_add {l2 :a 1 :b 2} {l2 :a 30 :b 40})]
    (printf c"sm %d %d  l2 %ld %ld\n" s.c s.s l.a l.b))
  (let [r (big_rev {big :v {(array i32 5) 1 2 3 4 5}})] [n (f3_neg {f3 :v {(array f32 3) 1.0 -2.0 3.5}})]
    (printf c"big %d %d %d %d %d  f3 %g %g %g\n" r.v[0] r.v[1] r.v[2] r.v[3] r.v[4]
            (cast f64 n.v[0]) (cast f64 n.v[1]) (cast f64 n.v[2])))
  (printf c"many_ints %ld\n" (many_ints 1 2 3 4 5 6 {l2 :a 7 :b 8}))
  (printf c"many_doubles %g\n" (many_doubles 1.0 2.0 3.0 4.0 5.0 6.0 7.0 {d2 :a 8.0 :b 9.0} 10.0))
  (printf c"partial %ld\n" (partial 1 2 3 4 5 {l2 :a 6 :b 7} 8))
  (printf c"small %d\n" (small 200 -100))
  (printf c"apply_v3 %g\n" (apply_v3 len2 {v3 :x 1.0 :y 2.0 :z 2.0}))
  (let [r (apply_big rot {big :v {(array i32 5) 1 2 3 4 5}})]
    (printf c"apply_big %d %d %d %d %d\n" r.v[0] r.v[1] r.v[2] r.v[3] r.v[4]))
  ; bit fields: written by C, read by lisc
  (let [x {bf}]
    (bf_set (addr x))
    (printf c"bf %u %u %d %u %d %d  sum %ld\n" x.a x.b x.c x.d (cast i32 x.e) x.tail (bf_sum x))
    ; written by lisc (neighbours must survive), read by C
    (set x.c 63)
    (set x.b 31)
    (set x.e false)
    (printf c"bf after %u %u %d %u %d %d  sum %ld\n" x.a x.b x.c x.d (cast i32 x.e) x.tail (bf_sum x)))
  ; the same layout declared in lisc
  (let [y {mybf :a 5 :b 17 :c -33 :d 100000 :e true :tail -2}]
    (printf c"mybf size %lu/%lu  sum %ld\n" (sizeof mybf) (sizeof bf) (bf_sum (deref (cast (ptr bf) (addr y))))))
  (printf c"global bf sums %ld %ld\n" (bf_sum (deref (cast (ptr bf) (addr gbf)))) (bf_sum gcbf))
  ; long double and __int128, alone and in structs
  (let [x (ld_mul 1.25 3.0)] [big (i128_mul 4000000000000 5000000000000)] [sh (u128_shift 1 100)]
       [w (w1_twice {w1 :v big})] [n {ldn :x 0.5 :n 3}]
    (printf c"ld %.20Lf %Lg %lu\n" x (math::sqrtl 2.0) (sizeof f80))
    (printf c"i128 %ld %lu %ld %ld %lu\n" (cast i64 (/ big 1000000000000000)) (cast u64 (>> sh 64))
            (cast i64 (/ w.v 1000000000000000)) (wide_low {wide :v (<< (cast i128 7) 64) :tag 5}) (sizeof i128))
    (printf c"ldn %Lg\n" (ldn_sum n n)))
  ; structs through ... : in registers, then (more than fit) on the stack
  (let [v {v3 :x 1.0 :y 2.0 :z 3.0}] [d {d2 :a 5.0 :b 1.0}] [m {mix :i 2 :f 0.5}]
       [l {l2 :a 9 :b 2}] [bg {big :v {(array i32 5) 1 0 0 0 7}}]
    (printf c"var structs %.1f %.1f\n" (var_structs c"vdmlbi" v d m l bg 3) (var_structs c"dddddvvmlll" d d d d d v v m l l l)))
  ; simd: C's vector types, in an SSE register or (32 bytes, no AVX) in memory
  (let [r (v4f_mad {(simd f32 4) 1.0 2.0 3.0 4.0} (splat (simd f32 4) 0.5) 2.0)]
       [i (cabi::v8i_sum {(simd i32 8) 1 1 1 1 1 1 1 1} 100 (splat (simd i32 8) 2))]
    (println "simd " r " " i " " (sizeof cabi::v4f) " " (alignof cabi::v8i)))
  0)
