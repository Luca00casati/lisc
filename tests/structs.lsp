; structs, unions, arrays, pointers, function pointers
(extern printf ((ptr i8) ...) i32)

(struct v3 [x f32] [y f32] [z f32])
(struct ray [origin v3] [dir v3] [tmax f32])
(struct node [value i32] [next (ptr node)])   ; refers to itself through a pointer
(struct mixed [a u8] [b u32] [c u16] [d f64]) ; C padding: 1+3, 4, 2+6, 8
(union bits [f f32] [u u32])

(defn v3-add [a v3] [b v3] -> v3             ; structs by value
  (return {v3 :x (+ a.x b.x) :y (+ a.y b.y) :z (+ a.z b.z)}))

(defn dot [a v3] [b v3] -> f32
  (return (+ (* a.x b.x) (* a.y b.y) (* a.z b.z))))

(defn scale [v (ptr v3)] [k f32]      ; through a pointer: v.x auto-derefs
  (set v.x (* v.x k))
  (set v.y (* v.y k))
  (set v.z (* v.z k)))

(defn sum [xs (ptr i32)] [n i32] -> i32
  (let [s 0]
    (for [i 0 n] (set s (+ s xs[i])))
    (return s)))

(defn twice [f (ptr (fn (i32) i32))] [x i32] -> i32 (return (f (f x))))
(defn inc [x i32] -> i32 (return (+ x 1)))

(defn main [] -> i32
  (let [r {ray :origin {v3 :x 1.0 :y 2.0 :z 3.0} :tmax 100.0}]
    (set r.dir.z -1.0)
    (let [s (v3-add r.origin r.dir)]
      (printf c"%g %g %g  tmax %g\n" (cast f64 s.x) (cast f64 s.y) (cast f64 s.z) (cast f64 r.tmax)))
    (scale (addr r.origin) 2.0)
    (printf c"dot %g\n" (cast f64 (dot r.origin r.origin))))
  ; arrays: in structs, decaying to pointers, indexed through pointers
  (let [xs {(array i32 5) 1 2 3 4 5}] [p (ptr i32) null]
    (set xs[4] 50)
    (set p (+ (addr xs[0]) 1))
    (printf c"sum %d  p[0] %d  p[1] %d  distance %ld\n" (sum xs 5) p[0] (deref (+ p 1)) (- (+ p 2) p)))
  ; a linked list on the stack
  (let [c {node :value 3}] [b {node :value 2}] [a {node :value 1}] [total 0] [it (ptr node) null]
    (set b.next (addr c))
    (set a.next (addr b))
    (set it (addr a))
    (while it
      (set total (+ total it.value))
      (set it it.next))
    (printf c"list %d\n" total))
  ; C layout
  (printf c"sizeof v3 %lu ray %lu mixed %lu align %lu  offsets b %lu\n"
          (sizeof v3) (sizeof ray) (sizeof mixed) (alignof mixed) (sizeof (array u8 3)))
  (let [u {bits :f 1.0}]
    (printf c"union %08x size %lu\n" u.u (sizeof bits)))
  (printf c"twice %d\n" (twice inc 40))
  (return 0))
