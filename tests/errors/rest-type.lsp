; error: argument: expected i32, got str
(defn sum [xs ..i32] -> i32 (return 0))
(defn main [] -> i32 (return (sum 1 "two")))
