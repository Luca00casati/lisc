; [x T] bindings, {T :field v} struct literals, a[i] indexing
(import "libc")
(using libc)

(struct point [x f32] [y f32])
(struct (pair A B) [a A] [b B])
(struct line [from point] [to point])

(defop + [p point] [q point] point {point :x (+ p.x q.x) :y (+ p.y q.y)})

(defn (swap A B) [p (pair A B)] (pair B A) {(pair B A) :a p.b :b p.a})

(defn sum [xs (ptr i32)] [n i32] i32
  (let [s 0]
    (for [i 0 n] (+= s xs[i]))
    s))

(defn answer [] i32 42)
(defn three [] (array i32 3) {(array i32 3) 1 2 3})

(def origin point {point :x 0.5 :y 0.25})   ; a constant global

; templates keep brackets; ,v[i] is (aref ,v i)
(defmacro swap! (v i j)
  `(let [tmp ,v[,i]]
     (set ,v[,i] ,v[,j])
     (set ,v[,j] tmp)))

; compile-time code takes them too
(defmacro twice (x) (let [n 2] `(* ,n ,x)))

(defn main [] i32
  (let [a (array i32 5)]
       [grid (array (array i32 3) 3)]
       [pts (array point 2)]
       [l {line :to {point :x 3.0 :y 4.0}}]
       [k u64 1]
    (for [i 0 5] (set a[i] (* i i)))
    (swap! a 0 4)
    (+= a[(+ k 1)] 100)
    (for [y 0 3] (for [x 0 3] (set grid[y][x] (+ (* y 10) x))))
    (set pts[1].y 7.5)
    (set pts[0] (+ origin {point :x 1.0 :y 1.0}))
    (printf c"%d %d %d %d %d  sum %d\n" a[0] a[1] a[2] a[3] a[4] (sum (addr a[0]) 5))
    (printf c"grid %d %d  pts %g %g %g  l.to.y %g\n" grid[2][1] grid[1][2]
            (cast f64 pts[0].x) (cast f64 pts[0].y) (cast f64 pts[1].y) (cast f64 l.to.y))
    (let [q (swap {(pair i32 f64) :a 7 :b 0.5})]
      (printf c"%g %d  %d %d  %d\n" q.a q.b (answer) (twice 21) (three)[2])))
  0)
