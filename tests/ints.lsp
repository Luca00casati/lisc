; integer conveniences: typed for counters, sizeof as an untyped constant, mixed shift counts
(import "libc")
(using libc)
(defn main [] i32
  (let [n u32 5] [sum u32 0] [buf (cast (ptr u32) (malloc (* (sizeof u32) (cast u64 n))))]
    (for [i u32 0 n] (set buf[i] (* i i)))
    (for [i u32 0 n 2] (set sum (+ sum buf[i])))
    (printf c"sum %u  size %u\n" sum (* (sizeof u32) n))
    (let [bits u64 1] [k i32 40] [m u8 3]
      (printf c"%lu %lu\n" (<< bits k) (>> (<< bits k) m)))
    (free buf))
  0)
