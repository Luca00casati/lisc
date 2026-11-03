; abort: call of a dropped heap-fn
(defn main [] i32
  (let [k 5] [f (fn :heap [x i32] i32 (+ x k))]
    (println (f 1))
    (drop f)
    (println (f 2)))
  0)
