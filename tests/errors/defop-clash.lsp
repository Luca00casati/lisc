; error: area is both a function and a defop
(defop area [r f32] -> f32 (return (* r r)))
(defn area [r f64] -> f64 (return r))
(defn main [] -> i32 (return 0))
