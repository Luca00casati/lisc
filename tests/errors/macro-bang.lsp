; error: twice is a macro: write twice!
(defmacro twice! (x) `(* 2 ,x))
(defn main [] -> i32 (return (twice 2)))
