; the core language: arithmetic, control flow, functions, globals, constants
(extern printf ((ptr i8) ...) i32)

(const N 10)
(const SCALE 2.5)                 ; untyped: f32 or f64 where it is used
(def counter i32 0)

(defn fib ((n i32)) i32
  (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))

(defn bump () void
  (set counter (+ counter 1)))

(defn sum-to ((n i64)) i64
  (let ((i i64 1))
    (let ((acc i64 0))
      (while (<= i n)
        (set acc (+ acc i))
        (set i (+ i 1)))
      acc)))

(defn classify ((x i32)) (ptr i8)
  (if (< x 0) c"negative" (if (= x 0) c"zero" c"positive")))

(defn main () i32
  (printf c"fib %d = %d\n" N (fib N))
  (bump) (bump) (bump)
  (printf c"counter %d\n" counter)
  (printf c"sum %ld\n" (sum-to 100000))
  ; for, continue, break
  (let ((odd 0))
    (for (i 0 100)
      (when (= (% i 2) 0) (continue))
      (when (> i 20) (break))
      (set odd (+ odd i)))
    (printf c"odd sum %d\n" odd))
  ; untyped literals round once, straight to the type they meet
  (let ((x f32 0.1) (y f64 0.1))
    (printf c"%.12g %.12g\n" (cast f64 x) y)
    (printf c"%.9g\n" (cast f64 (* x SCALE))))
  ; integers: signed and unsigned division, shifts, bit ops
  (let ((a i32 -7) (b u32 0xfffffff9))
    (printf c"%d %d %u %u\n" (/ a 2) (% a 2) (/ b 2) (>> b 28))
    (printf c"%d %d\n" (>> a 1) (& (~ a) 0xff)))
  ; casts and bit reinterpretation
  (printf c"%d %u 0x%08x\n" (cast i32 3.99) (truncate u8 300) (bitcast u32 (cast f32 1.0)))
  (printf c"%s %s %s\n" (classify -3) (classify 0) (classify 8))
  ; if as a value: an untyped branch takes the other's type
  (let ((k i32 3) (big (> k 2)))
    (printf c"%g %d %g\n" (if big 1 2.5) (if big k 0) (cast f64 (if big (cast f32 0.5) 2))))
  ; short-circuit logic: the second call must not run
  (let ((hits 0))
    (when (or (> 1 0) (do (set hits 1) true)) (printf c"or ok %d\n" hits)))
  0)
