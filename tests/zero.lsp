; everything starts as zero; null is the zero value of any type; undefined opts out
(import "libc")
(using libc)
(struct p3 [x f32] [y f32] [n i32] [next (ptr p3)])
(def g p3)                                     ; globals: zero
(def scratch (array u8 1024) undefined)        ; left as the loader gives it (bss anyway)

(defn main [] -> i32
  (let [a i32] [v p3] [arr (array f64 4)]       ; zero, without a value
    (printf c"zero: %d %g %g %d %p %g %d\n" a (cast f64 v.x) (cast f64 v.y) v.n (cast (ptr void) v.next) arr[3] g.n))
  (let [v {p3 :x 1.5 :n 7}]
    (set v null)                                ; back to all zero
    (let [k i32 9] [f f32 2.0] [ok true]
      (set k null) (set f null) (set ok null)
      (printf c"null: %g %d  %d %g %d\n" (cast f64 v.x) v.n k (cast f64 f) (cast i32 ok))))
  (let [q (ptr p3) null]
    (printf c"compare with null: %d %d\n" (cast i32 (= q null)) (cast i32 (!= null q))))
  (let [buf (array u8 64) undefined] [n u32 undefined]
    (memset buf 65 63)                          ; fills it before any use
    (set buf[63] 0)
    (set n (cast u32 (strlen (cast (ptr i8) (addr buf[0])))))
    (printf c"undefined, then filled: %u\n" n))
  (return 0))
