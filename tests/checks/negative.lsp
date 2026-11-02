; abort: index -1 out of bounds (length 3)
(defn main () i32
  (let ((xs (slice-of (make (array i32 3) 1 2 3))) (i i32 -1))
    (aref xs i)))
