; error: a simd's length is a power of two
(variant shape (circle [r f32]) (rect [w f32] [h f32]) none)
(variant (option T) (some [value T]) none)
(defn main [] i32 (discard (splat (simd f32 3) 1.0)) 0)
