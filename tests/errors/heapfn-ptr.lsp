; error: a (fn :heap ...) is a heap-fn, not a function pointer
(defn apply [f (ptr (fn (i32) i32))] [x i32] -> i32 (return (f x)))
(defn main [] -> i32 (return (apply (fn :heap [x i32] -> i32 (return x)) 1)))
