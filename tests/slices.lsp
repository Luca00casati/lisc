; slices: (slice T) is data + len; slice-of an array, a slice or a str
(import "libc")
(using libc)

(defn sum [xs (slice i32)] -> i64
  (let [s i64 0]
    (for [i u64 0 xs.len] (+= s xs[i]))
    (return s)))

(defn main [] -> i32
  (let [arr {(array i32 6) 1 2 3 4 5 6}] [all (slice-of arr)] [mid (slice-of arr 2 5)] [tail (slice-of all 4)]
       [s "hello, world"] [w (slice-of s 7)]
    (set mid[0] 30)                       ; a slice shares the array's memory
    (printf c"all %ld (%lu)  mid %ld (%lu)  tail %ld  arr[2] %d\n" (sum all) all.len (sum mid) mid.len (sum tail) arr[2])
    (printf c"%.*s (%lu bytes)\n" (cast i32 w.len) w.data w.len))
  (return 0))
