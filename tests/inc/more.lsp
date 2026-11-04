(defn inc [x (ptr i32)] (+= (deref x) 1))
(defn thrice-minus [x i32] -> i32 (return (- (* 3 x) x)))
