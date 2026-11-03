; generic structs and functions, specialised per list of types
(import "libc")
(using libc)

(struct (pair A B) [a A] [b B])
(struct (vec T) [data (ptr T)] [len u64] [cap u64])
(struct (node T) [value T] [next (ptr (node T))])   ; points to itself

(defn (push T) [v (ptr (vec T))] [x T] void
  (when (= v.len v.cap)
    (set v.cap (if (= v.cap 0) 4 (* v.cap 2)))
    (set v.data (cast (ptr T) (realloc v.data (* v.cap (sizeof T))))))
  (set v.data[v.len] x)
  (+= v.len 1))

(defn (sum T) [v (ptr (vec T))] T
  (let [s T null]                              ; (zero, whatever T is)
    (for [i u64 0 v.len] (+= s v.data[i]))
    s))

(defn (biggest (T >)) [a T] [b T] T (if (> a b) a b))   ; T must have >
(defn (swap A B) [p (pair A B)] (pair B A) {(pair B A) :a p.b :b p.a})
(defn (size-of T) [] u64 (sizeof T))
(defn (length T) [n (ptr (node T))] i32       ; recursive, and calling itself by inference
  (if (= n null) 0 (+ 1 (length n.next))))

(defn (apply-twice T) [f (ptr (fn (T) T))] [x T] T (f (f x)))
(defn (inc T) [x T] T (+ x 1))

(struct point [x f32] [y f32])
(defop + [p point] [q point] point {point :x (+ p.x q.x) :y (+ p.y q.y)})

(defn main [] i32
  (let [ints {(vec i32)}] [floats {(vec f64)}] [pts {(vec point)}]
    (for [i 0 10] (push (addr ints) i))
    (push (addr floats) 1.5)
    (push (addr floats) 2.25)
    (push (addr pts) {point :x 1.0 :y 2.0})
    (push (addr pts) {point :x 3.0 :y 4.0})
    ((push i32) (addr ints) 100)                ; the types written out
    (let [p (sum (addr pts))]
      (printf c"%d %g %g %g\n" (sum (addr ints)) (sum (addr floats)) (cast f64 p.x) (cast f64 p.y)))
    (free ints.data) (free floats.data) (free pts.data))
  (printf c"%d %g %lu\n" (biggest 3 7) (biggest 2.5 -1.0) (biggest (cast u64 9) 4))
  (let [p {(pair i32 f64) :a 7 :b 0.5}] [q (swap p)]
    (printf c"%g %d %lu %lu\n" q.a q.b (sizeof (pair i32 f64)) ((size-of (pair u8 (vec u8))))))
  (let [c {(node i32) :value 3}] [b {(node i32) :value 2 :next (addr c)}] [a {(node i32) :value 1 :next (addr b)}]
    (printf c"%d\n" (length (addr a))))
  ; generic functions as values: written out, or told by the pointer type wanted
  (let [f (addr (inc i64))] [g (ptr (fn (f32) f32)) inc]
    (printf c"%ld %g %d %g\n" (f 41) (cast f64 (g 0.5)) (apply-twice (addr (inc i32)) 5) (apply-twice inc 2.5)))
  0)
