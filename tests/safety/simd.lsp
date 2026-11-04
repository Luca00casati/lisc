; abort: index 7 out of bounds (length 4)
(defn main [] -> i32
  (let [s {(simd f32 4) 1 2 3 4}] [i 7]
    (println s[i]))
  (return 0))
