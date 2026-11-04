; std/sort and std/map
(import "libc")
(import "std/sort")
(import "std/map")
(import "std/vec")
(using libc sort vec)

(struct pt [x i32] [y i32])
(defop < [a pt] [b pt] -> bool (return (or (< a.x b.x) (and (= a.x b.x) (< a.y b.y)))))
(defn by-len [a str] [b str] -> bool (return (< a.len b.len)))

(defn main [] -> i32
  ; sort numbers, structs by their <, strs by a function
  (let [v {(vec i32)}] [seed u32 12345] [ok true]
    (for [_i 0 1000]
      (set seed (+ (* seed 1103515245) 12345))
      (push (addr v) (cast i32 (% (>> seed 8) 1000))))
    (sort (items (addr v)))
    (for [i u64 1 v.len] (when (> v.data[(- i 1)] v.data[i]) (set ok false)))
    (printf c"1000 ints sorted: %d, first %d last %d\n" (cast i32 ok) v.data[0] v.data[999])
    (release (addr v)))
  (let [ps {(array pt 5) {pt :x 2 :y 1} {pt :x 1 :y 9} {pt :x 2 :y 0} {pt :x 0 :y 5} {pt :x 1 :y 1}}]
    (sort {(slice pt) :data (addr ps[0]) :len 5})
    (for [i 0 5] (printf c"(%d %d) " (. ps[i] x) (. ps[i] y)))
    (printf c"\n"))
  (let [ws {(array str 4) "teapot" "a" "kettle!" "cup"}]
    (sort-by {(slice str) :data (addr ws[0]) :len 4} by-len)
    (for [i 0 4] (printf c"%.*s " (cast i32 (. ws[i] len)) (. ws[i] data)))
    (printf c"\n"))
  ; maps: integer keys through growth and removal, str keys, struct keys
  (let [m {(map::map i32 i32)}] [ok true]
    (for [i 0 10000] (map::put (addr m) i (* i i)))
    (for [i 0 10000 2] (when (not (map::remove (addr m) i)) (set ok false)))
    (for [i 0 10000]
      (let [p (map::get (addr m) i)]
        (when (!= (= p null) (= (% i 2) 0)) (set ok false))
        (when (and (!= p null) (!= (deref p) (* i i))) (set ok false))))
    (printf c"int map: %lu left, all right: %d\n" m.len (cast i32 ok))
    (map::release (addr m)))
  (let [words {(map::map str i32)}] [text {(array str 7) "the" "pot" "the" "lid" "pot" "the" "spout"}]
    (for [i 0 7]
      (let [p (map::get (addr words) text[i])]
        (if (= p null) (map::put (addr words) text[i] 1) (+= (deref p) 1))))
    (printf c"the %d pot %d lid %d spout %d kettle %d, %lu words\n" (deref (map::get (addr words) "the")) (deref (map::get (addr words) "pot"))
            (deref (map::get (addr words) "lid")) (deref (map::get (addr words) "spout")) (cast i32 (map::has (addr words) "kettle")) words.len)
    (let [total 0]
      (map::each (addr words) (fn [_k str] [n i32] (+= total n)))
      (printf c"total %d\n" total))
    (map::release (addr words)))
  (let [grid {(map::map pt f64)}]
    (map::put (addr grid) {pt :x 3 :y 4} 5.0)
    (map::put (addr grid) {pt :x 4 :y 3} 7.5)
    (printf c"%g %g %d\n" (deref (map::get (addr grid) {pt :x 3 :y 4})) (deref (map::get (addr grid) {pt :x 4 :y 3}))
            (cast i32 (map::has (addr grid) {pt :x 3 :y 3})))
    (map::release (addr grid)))
  (return 0))
