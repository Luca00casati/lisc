; a function gives its result only with (return e); one without -> R gives nothing
(defn sign [x i32] -> i32
  (if (< x 0) (return -1) (return 1)))       ; every path returns: nothing after the if
(defn first-big [xs (slice i32)] -> i32
  (for [x :in xs]
    (when (> x 10) (return x)))
  (return 0))
(defn count-up [] -> i32                      ; (while true ...) is left only by return
  (let [i 0]
    (while true
      (+= i 1)
      (when (> i 5) (return i)))))
(defn hello [] (println "hello"))             ; no result
(defn main [] -> i32
  (hello)
  (let [a {(array i32 3) 3 30 4}]
       [x (if (> (sign -5) 0) 1 2)]           ; blocks are still expressions
       [f (fn [x i32] -> i32 (return (* x 2)))]
    (println (sign -5) " " (first-big (slice-of a)) " " (count-up) " " x " " (f 21)))
  (return 0))
