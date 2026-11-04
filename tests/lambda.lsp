; (lambda [a T] ... -> R body...): a function written where it is used;
; (lambda [a T] ... -> e): the short form, e is the result (its type inferred)
(import "std/sort")
(using sort)

(defn apply [f (ptr (fn (i32) i32))] [x i32] -> i32 (return (f x)))
(defn (map-in-place T) [xs (slice T)] [f (ptr (fn (T) T))]
  (for [x :in xs] (set x (f x))))
(defn (twice-each T) [xs (slice T)]
  (map-in-place xs (lambda [x T] -> (+ x x))))       ; in a generic: T is the instance's

(def counter i32 0)

(defn main [] -> i32
  (let [a {(array i32 6) 5 3 8 1 9 2}]
       [inc (lambda [x i32] -> (+ x 1))]
       [hello (lambda [] (println "hello from a fn"))]
    (sort-by (slice-of a) (lambda [p i32] [q i32] -> (> p q)))   ; descending
    (println a " " (apply inc 41) " " (apply (lambda [x i32] -> (* x x)) 7))
    (twice-each (slice-of a))
    (println a)
    (hello)
    (let [bump (lambda [] (+= counter 1))]     ; globals are fine
      (bump) (bump))
    (let [adder (lambda [n i32] -> (ptr (fn (i32) i32))     ; a fn making a fn
                  (if (> n 0) (return (lambda [x i32] -> (+ x 100))) (return (lambda [x i32] -> (- x 100)))))]
      (println counter " " ((adder 1) 1) " " ((adder -1) 1))))
  (closures)
  (short-forms)
  (return 0))

; closures: a fn using locals around it captures them, by reference
(defn run [f (closure () void)] (f))
(defn plus1 [x i32] -> i32 (return (+ x 1)))
(defn apply-c [f (closure (i32) i32)] [x i32] -> i32 (return (f x)))
(defn (fold T) [xs (slice T)] [init T] [f (closure (T T) T)] -> T
  (let [acc init]
    (for [x :in xs] (set acc (f acc x)))
    (return acc)))

(defn closures []
  (let [k 10]
       [n 0]
       [p {(array f32 2) 1.5 2}]
       [a {(array i32 5) 1 2 3 4 5}]
       [calls 0]
    (println (apply-c (lambda [x i32] -> (+ x k)) 5) " " (apply-c plus1 1))   ; a plain fn is a closure too
    (run (lambda []                       ; one in another
           (run (lambda [] (+= n 10)))
           (+= n 1)))
    (println n " " (fold (slice-of a) 0 (lambda [s i32] [x i32] -> (+ s (* x n)))))
    (let [f (lambda [s f32] -> (* s (+ p[0] p[1])))]
      (set p[0] 0.5)                       ; seen: a capture is the variable itself
      (println (f 2.0)))
    (let [on (lambda [v i32] (set k v))]
      (run (lambda [] (on 7)))           ; a closure captured by another
      (println k))
    (sort-by (slice-of a) (lambda [x i32] [y i32] -> bool (+= calls 1) (return (> x y))))
    (println a " " (> calls 0))
    (for [i 0 3]
      (run (lambda [] (print i " "))))
    (println)))

; the short form's result type: the one wanted, else the expression's
(defn short-forms []
  (let [half (lambda [x f64] -> (/ x 2.0))]              ; f64, from x
       [big (lambda [x i64] -> (> x 1000))]              ; bool
       [say (lambda [s str] -> (println "say " s))]       ; void
       [to-f (lambda [x i32] -> (cast f32 x))]
    (say "hi")
    (println (half 3.0) " " (big 5000) " " (to-f 7) " " (apply-c (lambda [x i32] -> (* x 3)) 5))))
