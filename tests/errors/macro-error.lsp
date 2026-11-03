; error: only symbols here, got (1 2)
(defmacro must-sym! (x) (if (symbol? x) x (error c"only symbols here, got " x)))
(defn main [] i32 (must-sym! (1 2)) 0)
