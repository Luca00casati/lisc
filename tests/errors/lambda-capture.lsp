; error: this fn uses 'k' from the code around it, so it is a closure, not a function pointer
(defn apply [f (ptr (fn (i32) i32))] [x i32] i32 (f x))
(defn main [] i32
  (let [k 3] (apply (fn [x i32] i32 (+ x k)) 1)))
