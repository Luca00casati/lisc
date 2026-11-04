; error: note: in (half str), used here
(defn (half T) [x T] -> T (return (/ x 2)))
(defn main [] -> i32 (half "no") (return (half 4)))
