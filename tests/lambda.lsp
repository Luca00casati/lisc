; (fn [a T] ... R body...): a function written where it is used
(import "std/sort")
(using sort)

(defn apply [f (ptr (fn (i32) i32))] [x i32] i32 (f x))
(defn (map-in-place T) [xs (slice T)] [f (ptr (fn (T) T))] void
  (for [x :in xs] (set x (f x))))
(defn (twice-each T) [xs (slice T)] void
  (map-in-place xs (fn [x T] T (+ x x))))       ; in a generic: T is the instance's

(def counter i32 0)

(defn main [] i32
  (let [a {(array i32 6) 5 3 8 1 9 2}]
       [inc (fn [x i32] i32 (+ x 1))]
       [hello (fn [] void (println "hello from a fn"))]
    (sort-by (slice-of a) (fn [p i32] [q i32] bool (> p q)))   ; descending
    (println a " " (apply inc 41) " " (apply (fn [x i32] i32 (* x x)) 7))
    (twice-each (slice-of a))
    (println a)
    (hello)
    (let [bump (fn [] void (+= counter 1))]     ; globals are fine
      (bump) (bump))
    (let [adder (fn [n i32] (ptr (fn (i32) i32))     ; a fn making a fn
                  (if (> n 0) (fn [x i32] i32 (+ x 100)) (fn [x i32] i32 (- x 100))))]
      (println counter " " ((adder 1) 1) " " ((adder -1) 1))))
  (closures)
  0)

; closures: a fn using locals around it captures them, by reference
(defn run [f (closure () void)] void (f))
(defn plus1 [x i32] i32 (+ x 1))
(defn apply-c [f (closure (i32) i32)] [x i32] i32 (f x))
(defn (fold T) [xs (slice T)] [init T] [f (closure (T T) T)] T
  (let [acc init]
    (for [x :in xs] (set acc (f acc x)))
    acc))

(defn closures [] void
  (let [k 10]
       [n 0]
       [p {(array f32 2) 1.5 2}]
       [a {(array i32 5) 1 2 3 4 5}]
       [calls 0]
    (println (apply-c (fn [x i32] i32 (+ x k)) 5) " " (apply-c plus1 1))   ; a plain fn is a closure too
    (run (fn [] void                       ; one in another
           (run (fn [] void (+= n 10)))
           (+= n 1)))
    (println n " " (fold (slice-of a) 0 (fn [s i32] [x i32] i32 (+ s (* x n)))))
    (let [f (fn [s f32] f32 (* s (+ p[0] p[1])))]
      (set p[0] 0.5)                       ; seen: a capture is the variable itself
      (println (f 2.0)))
    (let [on (fn [v i32] void (set k v))]
      (run (fn [] void (on 7)))           ; a closure captured by another
      (println k))
    (sort-by (slice-of a) (fn [x i32] [y i32] bool (+= calls 1) (> x y)))
    (println a " " (> calls 0))
    (for [i 0 3]
      (run (fn [] void (print i " "))))
    (println)))
