; error: this match misses shape.rect (or add an else)
(variant shape (circle [r f32]) (rect [w f32] [h f32]) none)
(variant (option T) (some [value T]) none)
(defn main [] -> i32 (match (shape.circle 1.0) ((circle r) (return 0)) (none (return 1))))
