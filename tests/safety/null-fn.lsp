; abort: call through a null function pointer
(defn main [] -> i32
  (let [f (ptr (fn (i32) i32)) null]
    (return (f 1))))
