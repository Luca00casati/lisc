; warning: the result of twice is not used
(defn twice ((x i32)) i32 (* 2 x))
(defn main () i32
  (twice 1)              ; warned
  (discard (twice 2))    ; meant: not warned
  (twice 3))
