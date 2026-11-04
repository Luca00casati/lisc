; error: a closure can't be returned
(defn adder [n i32] -> (closure (i32) i32) (return (fn [x i32] -> i32 (return (+ x n)))))
(defn main [] -> i32 (return 0))
