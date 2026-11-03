; error: a heap-fn can't capture 'f', a closure
(defn keep [f (closure () void)] (heap-fn () void)
  (fn :heap [] void (f)))
(defn main [] i32 0)
