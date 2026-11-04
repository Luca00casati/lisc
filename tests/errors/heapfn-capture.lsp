; error: a heap-fn can't capture 'f', a closure
(defn keep [f (closure () void)] -> (heap-fn () void)
  (return (lambda :heap [] (f))))
(defn main [] -> i32 (return 0))
