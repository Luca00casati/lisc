; structs by value across the C calling convention, both ways, and bit fields
(import "libc")
(using libc)
(import-c "abi.h" :as cabi)
(using cabi)

; a lisc struct laid out like C's struct bf, bit fields included
(struct mybf (a u32 :bits 3) (b u32 :bits 5) (c i32 :bits 7) (d u32 :bits 17) (e bool :bits 1) (tail i8))

(defn len2 ((v v3)) f32 (+ (* v.x v.x) (* v.y v.y) (* v.z v.z)))   ; called from C
(defn rot ((b big)) big                                              ; called from C, big result
  (make big :v (make (array i32 5) (aref b.v 1) (aref b.v 2) (aref b.v 3) (aref b.v 4) (aref b.v 0))))

(defn main () i32
  (let ((v (v3_scale (make v3 :x 1.0 :y 2.0 :z 3.0) 0.5)))
    (printf c"v3 %g %g %g\n" (cast f64 v.x) (cast f64 v.y) (cast f64 v.z)))
  (let ((d (d2_swap (make d2 :a 1.5 :b 2.25))))
    (printf c"d2 %g %g sum %g\n" d.a d.b (d2_sum d)))
  (let ((m (mix_make -7 0.25)) (x (fi_twice (make fi :f 1.5 :i 21 :d 0.125))))
    (printf c"mix %d %g  fi %g %d %g\n" m.i (cast f64 m.f) (cast f64 x.f) x.i x.d))
  (let ((s (sm_inc (make sm :c 9 :s -300))) (l (l2_add (make l2 :a 1 :b 2) (make l2 :a 30 :b 40))))
    (printf c"sm %d %d  l2 %ld %ld\n" s.c s.s l.a l.b))
  (let ((r (big_rev (make big :v (make (array i32 5) 1 2 3 4 5)))) (n (f3_neg (make f3 :v (make (array f32 3) 1.0 -2.0 3.5)))))
    (printf c"big %d %d %d %d %d  f3 %g %g %g\n" (aref r.v 0) (aref r.v 1) (aref r.v 2) (aref r.v 3) (aref r.v 4)
            (cast f64 (aref n.v 0)) (cast f64 (aref n.v 1)) (cast f64 (aref n.v 2))))
  (printf c"many_ints %ld\n" (many_ints 1 2 3 4 5 6 (make l2 :a 7 :b 8)))
  (printf c"many_doubles %g\n" (many_doubles 1.0 2.0 3.0 4.0 5.0 6.0 7.0 (make d2 :a 8.0 :b 9.0) 10.0))
  (printf c"partial %ld\n" (partial 1 2 3 4 5 (make l2 :a 6 :b 7) 8))
  (printf c"small %d\n" (small 200 -100))
  (printf c"apply_v3 %g\n" (apply_v3 len2 (make v3 :x 1.0 :y 2.0 :z 2.0)))
  (let ((r (apply_big rot (make big :v (make (array i32 5) 1 2 3 4 5)))))
    (printf c"apply_big %d %d %d %d %d\n" (aref r.v 0) (aref r.v 1) (aref r.v 2) (aref r.v 3) (aref r.v 4)))
  ; bit fields: written by C, read by lisc
  (let ((x (make bf)))
    (bf_set (addr x))
    (printf c"bf %u %u %d %u %d %d  sum %ld\n" x.a x.b x.c x.d (cast i32 x.e) x.tail (bf_sum x))
    ; written by lisc (neighbours must survive), read by C
    (set x.c 63)
    (set x.b 31)
    (set x.e false)
    (printf c"bf after %u %u %d %u %d %d  sum %ld\n" x.a x.b x.c x.d (cast i32 x.e) x.tail (bf_sum x)))
  ; the same layout declared in lisc
  (let ((y (make mybf :a 5 :b 17 :c -33 :d 100000 :e true :tail -2)))
    (printf c"mybf size %lu/%lu  sum %ld\n" (sizeof mybf) (sizeof bf) (bf_sum (deref (cast (ptr bf) (addr y))))))
  0)
