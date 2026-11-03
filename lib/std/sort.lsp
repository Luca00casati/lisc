; std/sort — sorting slices in place.
;
;   (sort s)            by <: numbers, or a struct with a (defop < ...)
;   (sort-by s less)    by your own order: less is a function or a closure
;                       taking two T and giving a bool (a < b)
;
; Quicksort (Hoare partition, middle pivot), insertion sort for short runs;
; not stable.

(defn (sort-by T F) [s (slice T)] [less F] void
  (quick s.data 0 (cast i64 s.len) less))

(private
  (defn (lt (T <)) [a T] [b T] bool (< a b)))
(defn (sort (T <)) [s (slice T)] void (sort-by s (addr (lt T))))

; a[lo..hi)
(private
  (defn (quick T F) [a (ptr T)] [lo i64] [hi i64] [less F] void
    (while (> (- hi lo) 16)
      (let [p a[(+ lo (/ (- hi lo) 2))]] [i lo] [j (- hi 1)]
        (while (<= i j)
          (while (less a[i] p) (+= i 1))
          (while (less p a[j]) (-= j 1))
          (when (<= i j)
            (let [x a[i]]
              (set a[i] a[j])
              (set a[j] x))
            (+= i 1)
            (-= j 1)))
        ; the smaller side by recursion, the bigger one by the loop (depth stays log n)
        (if (< (- j lo) (- hi i))
            (do (quick a lo (+ j 1) less) (set lo i))
            (do (quick a i hi less) (set hi (+ j 1))))))
    (for [i i64 (+ lo 1) hi]
      (let [x a[i]] [j i]
        (while (and (> j lo) (less x a[(- j 1)]))
          (set a[j] a[(- j 1)])
          (-= j 1))
        (set a[j] x)))))
