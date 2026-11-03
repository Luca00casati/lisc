; error: shape.rect takes 2 values, got 1
(variant shape (circle [r f32]) (rect [w f32] [h f32]) none)
(variant (option T) (some [value T]) none)
(defn main [] i32 (discard (shape.rect 1.0)) 0)
