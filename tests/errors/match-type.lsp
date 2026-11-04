; error: match needs a variant, got i32
(variant shape (circle [r f32]) (rect [w f32] [h f32]) none)
(variant (option T) (some [value T]) none)
(defn main [] -> i32 (match 5 (else (return 0))))
