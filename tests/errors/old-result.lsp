; error: the result type goes after ->
(defn twice [x i32] i32 (* 2 x))
(defn main [] -> i32 (return (twice 2)))
