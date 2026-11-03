; error: for ... :in needs an array, simd, slice, str or a struct with data and len, got i32
(variant shape (circle [r f32]) (rect [w f32] [h f32]) none)
(variant (option T) (some [value T]) none)
(defn main [] i32 (for [x :in 5] (discard x)) 0)
