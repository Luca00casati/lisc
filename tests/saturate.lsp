; float to integer casts saturate: the nearest value the type has, NaN is 0
(defn main [] -> i32
  (let [big f64 3e9] [neg f32 -7.9] [nan (/ 0.0 (- big big))] [inf (/ 1.0 (- big big))] [x f80 1e30]
    (println (cast i32 big) " " (cast i32 (- big)) " " (cast u8 neg) " " (cast i8 neg) " " (cast u8 300.5))
    (println (cast i32 nan) " " (cast u64 inf) " " (cast i64 (- inf)) " " (cast i16 x) " " (cast u32 2.9)))
  (return 0))
