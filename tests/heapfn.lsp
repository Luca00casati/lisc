; heap-fn: closures that keep their own copies, on the heap, until dropped
(import "std/vec")
(import "std/sort")

(struct button [name str] [on-click (heap-fn (i32) void)])   ; allowed in a field

(defn make-greet [who str] [times (ptr i32)] -> (heap-fn (i32) void)   ; and as a result
  (return (fn :heap [x i32]
            (+= (deref times) 1)
            (println "hello " who " at " x))))

(defn counter [] -> (heap-fn () i32)
  (let [n 0]
    (return (fn :heap [] -> i32 (+= n 1) (return n)))))       ; n is its own copy, kept between calls

(defn plus1 [x i32] (println "plain " x))
(defn run [f (closure (i32) void)] [x i32] (f x))

(defn main [] -> i32
  (let [clicks 0]
       [ok {button :name "ok" :on-click (make-greet "ann" (addr clicks))}]
       [no {button :name "no" :on-click (make-greet "bob" (addr clicks))}]
       [hs (vec::vec (heap-fn (i32) void))]
       [c (counter)]
       [a {(array i32 4) 3 1 4 2}]
       [desc true]
    (ok.on-click 10)
    (no.on-click 20)
    (run ok.on-click 30)                ; passed down as a closure
    (println clicks " clicks")
    (for [i 0 3]
      (vec::push (addr hs) (fn :heap [x i32] (println "handler " i " got " x))))   ; each keeps its own i
    (vec::push (addr hs) plus1)                ; a plain function too
    (for [h :in hs] (h 7))
    (println (c) " " (c) " " (c))
    (let [cmp (fn :heap [x i32] [y i32] -> bool (if desc (return (> x y)) (return (< x y))))]
      (sort::sort-by (slice-of a) cmp)
      (println a)
      (drop cmp))
    (for [h :in hs] (drop h))           ; plus1's has nothing to free
    (drop ok.on-click)
    (drop no.on-click)
    (drop c)
    (vec::release (addr hs)))
  (return 0))
