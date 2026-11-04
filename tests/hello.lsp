; the smallest program: a hand-declared C function
(extern puts ((ptr i8)) i32)

(defn main [] -> i32
  (puts c"hello from lisc")
  (return 0))
