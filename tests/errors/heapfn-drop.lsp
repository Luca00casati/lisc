; error: drop frees a heap-fn's env, not (closure () void)
(defn main [] -> i32
  (let [k 1] [f (lambda [] (println k))]
    (drop f))
  (return 0))
