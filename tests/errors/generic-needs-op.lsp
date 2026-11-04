; error: needs (+ T T), and there is none for T = (ptr i8)
(defn (total (T +)) [a T] [b T] -> T (return (+ a b)))
(defn main [] -> i32 (total c"a" c"b") (return 0))
