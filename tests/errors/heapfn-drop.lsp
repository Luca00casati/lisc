; error: drop frees a heap-fn's env, not (closure () void)
(defn main [] i32
  (let [k 1] [f (fn [] void (println k))]
    (drop f))
  0)
