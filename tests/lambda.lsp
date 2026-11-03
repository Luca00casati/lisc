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
  0)
