; error: return inside a defer
(defn f () i32 (defer (return 1)) 0)
(defn main () i32 (f))
