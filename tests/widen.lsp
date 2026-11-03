; lossless integer widening is implicit; narrowing and sign changes need a cast
(import "libc")
(using libc)
(defn take64 [x u64] u64 (* x 2))
(defn main [] i32
  (let [a u8 200] [b u16 60000] [c u32 4000000000] [d i32 -5] [f f32 1.5]
    (printf c"%lu %lu %ld\n" (take64 a) (+ c b a) (+ d (cast i64 c)))
    (let [e i64 b] [g f64 f] [h i64 (* d a)]          ; u16 → i64, f32 → f64, i32 * u8 → i32
      (printf c"%ld %g %ld\n" e g h)))
  0)
