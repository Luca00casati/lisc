; abort: index 3 out of bounds (length 1)
(import "std/vec")
(defn main [] i32
  (let [v (vec::vec i32)]
    (vec::push (addr v) 1)
    (println v[0])
    (println v[3]))
  0)
