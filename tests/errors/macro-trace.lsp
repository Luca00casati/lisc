; error: note: in expansion of macro outer
(defmacro inner! (x) `(+ ,x 1.5))                ; wrong for an i32 argument
(defmacro outer! (y) `(* 2 (inner! ,y)))
(defn main [] -> i32
  (let [n i32 3]
    (return (outer! n))))
