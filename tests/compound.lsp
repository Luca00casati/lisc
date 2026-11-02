; compound assignment: the place is computed once; defops and bit fields work too
(import "libc")
(using libc)
(struct v2 (x f32) (y f32))
(defop + ((a v2) (b v2)) v2 (make v2 :x (+ a.x b.x) :y (+ a.y b.y)))
(struct flags (lo u8 :bits 3) (hi u8 :bits 5))
(def calls i32 0)
(defn idx () i32 (+= calls 1) 2)                 ; counts how often it runs

(defn main () i32
  (let ((a (make (array i32 4) 1 2 3 4)))
    (+= (aref a (idx)) 10)                         ; idx runs once
    (printf "a[2] %d  idx calls %d\n" (aref a 2) calls))
  (let ((n i32 100) (u u32 0xf0) (w u64 1))
    (-= n 1) (*= n 3) (/= n 2) (%= n 50)
    (&= u 0x3c) (|= u 1) (^= u 0xff) (<<= w 40) (>>= w 8)
    (+= w u)                                       ; a u32 widens into the u64
    (printf "%d %u %lu\n" n u w))
  (let ((v (make v2 :x 1.0 :y 2.0)) (f (make flags :lo 5 :hi 30)))
    (+= v (make v2 :x 0.5 :y 0.5))                 ; through the defop
    (+= f.lo 1) (-= f.hi 10)                       ; bit fields
    (printf "v %g %g  flags %u %u\n" (cast f64 v.x) (cast f64 v.y) f.lo f.hi))
  (let ((p (ptr i8) "abcdef"))
    (+= p 2)                                       ; pointer arithmetic
    (printf "p %s\n" p))
  0)
