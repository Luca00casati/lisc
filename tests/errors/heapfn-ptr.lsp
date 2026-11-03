; error: a (fn :heap ...) is a heap-fn, not a function pointer
(defn apply [f (ptr (fn (i32) i32))] [x i32] i32 (f x))
(defn main [] i32 (apply (fn :heap [x i32] i32 x) 1))
