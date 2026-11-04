; abort: call of a dropped heap-fn
(defn main [] -> i32
  (let [k 5] [f (lambda :heap [x i32] -> (+ x k))]
    (println (f 1))
    (drop f)
    (println (f 2)))
  (return 0))
