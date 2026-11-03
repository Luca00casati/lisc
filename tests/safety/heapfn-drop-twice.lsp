; abort: heap-fn dropped twice
(defn main [] i32
  (let [k 5] [f (fn :heap [x i32] i32 (+ x k))] [g f]   ; g is a copy: the same env
    (drop f)
    (drop g))
  0)
