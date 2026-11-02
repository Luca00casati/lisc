; abort: 300 does not fit in u8
(defn main () i32
  (let ((x i32 300)) (cast i32 (cast u8 x))))
