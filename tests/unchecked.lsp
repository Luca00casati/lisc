; (unchecked ...) and :unchecked: no bounds or division checks in them
(defn sum [xs (slice i32)] i32 :unchecked
  (let [s 0]
    (for [i u64 0 xs.len] (+= s xs[i]))
    s))
(defn main [] i32
  (let [a {(array i32 4) 1 2 3 4}] [i 2] [d 2]
    (println (sum (slice-of a)) " " (unchecked (+ a[i] (/ 10 d))))
    (let [f (unchecked (fn [x i32] i32 (/ x d)))]   ; a fn written inside is unchecked too
      (println (unchecked (f 9)))))
  0)
