; error: circle has 1 field, the pattern names 2
(variant shape (circle [r f32]) (rect [w f32] [h f32]) none)
(variant (option T) (some [value T]) none)
(defn main [] -> i32 (match shape.none ((circle a b) (return 0)) (else (return 1))))
