; error: sort needs (< T T), and there is none for T = pt: define it with defop
(import "std/sort")
(struct pt [x i32] [y i32])
(defn main [] i32
  (let [ps {(array pt 2)}]
    (sort::sort {(slice pt) :data (addr ps[0]) :len 2}))
  0)
