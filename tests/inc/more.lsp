(defmacro inc! (x) `(set ,x (+ ,x 1)))
(defn thrice-minus [x i32] -> i32 (return (- (* 3 x) x)))
