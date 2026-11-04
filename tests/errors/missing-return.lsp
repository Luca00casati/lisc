; error: sign can reach its end without (return e): it gives i32, only by return
(defn sign [x i32] -> i32
  (when (< x 0) (return -1))
  (when (> x 0) (return 1)))
(defn main [] -> i32 (return (sign 2)))
