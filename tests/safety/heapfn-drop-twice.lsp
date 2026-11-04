; abort: heap-fn dropped twice
(defn main [] -> i32
  (let [k 5] [f (lambda :heap [x i32] -> (+ x k))] [g f]   ; g is a copy: the same env
    (drop f)
    (drop g))
  (return 0))
