; abort: -5 does not fit in u32
(defn main () i32
  (let ((x i32 -5)) (cast i32 (+ (cast u32 x) 0))))
