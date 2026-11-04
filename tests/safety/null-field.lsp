; abort: null pointer dereference
(struct pt [x i32] [y i32])
(defn get-x [p (ptr pt)] -> i32 (return p.x))
(defn main [] -> i32 (return (get-x null)))
