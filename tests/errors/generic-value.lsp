; error: inc is generic: write (addr (inc T ...)) to say for which types
(defn (inc T) [x T] T (+ x 1))
(defn main [] i32 (let [f inc] 0))
