; (simd T N): numbers side by side in a vector register
(defn dot [a (simd f32 4)] [b (simd f32 4)] f32 (simd-sum (* a b)))

(defn main [] i32
  (let [a {(simd f32 4) 1.0 2.0 3.0 4.0}]
       [b (splat (simd f32 4) 0.5)]
       [i {(simd i32 4) 1 -2 3 -4}]
    (set a[3] 8.0)
    (let [c (+ (* a 2.0) b)]                   ; a number goes to every element
      (println c " " (shuffle c 3 2 1 0) " " (shuffle i 0 0) " dot " (dot a b)))
    (println (simd-hmin i) " " (simd-hmax i) " " (simd-abs i) " " (simd-sum i) " " (- i) " " (* i i))
    (println (simd-min a 2.5) " " (simd-max a 2.5) " " (simd-sqrt {(simd f32 4) 16.0 9.0}))
    (println (cast (simd f32 4) i) " " (cast (simd i32 4) (* a 1.5)) " " (bitcast (simd u32 4) (splat (simd f32 4) 1.0))[0])
    (println (& {(simd u8 8) 255 15 7} (splat (simd u8 8) 12)) " " (<< (splat (simd u16 4) 1) 3))
    (println (sizeof (simd f32 8)) " " (alignof (simd f64 2)) " " (sizeof (simd u8 16))))
  0)
