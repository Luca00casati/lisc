; abort: null pointer dereference
(defn main [] i32
  (let [p (ptr i32) null]
    p[3]))
