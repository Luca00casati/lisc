; for ... :in — each element of an array, simd, slice, str, (vec T), or through a pointer
(import "std/vec")
(using vec)

(struct ring [data (ptr i32)] [len u32] [cap u32])   ; anything with data and len

(defn total [xs (slice i32)] -> i32
  (let [s 0]
    (for [x :in xs] (+= s x))
    (return s)))

(defn main [] -> i32
  (let [a {(array i32 5) 5 3 8 1 9}] [v {(vec f64)}] [m {(array (array i32 3) 2) {(array i32 3) 1 2 3} {(array i32 3) 4 5 6}}]
    (for [x :in a] (*= x 10))                    ; x is the element itself
    (println a " " (total (slice-of a 1 4)))
    (for [i x :in a]
      (when (= i 1) (continue))
      (when (> x 80) (break))
      (print i "=" x " "))
    (println)
    (push (addr v) 1.5) (push (addr v) 2.5)
    (let [pv (addr v)]
      (for [x :in pv] (+= x 0.25)))               ; through the pointer
    (for [i x :in v] (print i ":" x " "))
    (for [b :in "hé"] (print b " "))               ; a str's bytes
    (for [row :in m] (for [x :in row] (print x)))
    (println)
    (let [r {ring :data (addr a[0]) :len 2}] (for [x :in r] (print x " ")))
    (for [x :in {(simd i32 4) 7 8 9 10}] (print x))
    (for [_ :in (slice-of a 0 0)] (print "never"))
    (println)
    (release (addr v)))
  (return 0))
