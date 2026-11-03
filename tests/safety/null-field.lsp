; abort: null pointer dereference
(struct pt [x i32] [y i32])
(defn get-x [p (ptr pt)] i32 p.x)
(defn main [] i32 (get-x null))
