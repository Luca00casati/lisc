; error: a closure can't be set again
(defn run [f (closure () void)] void (f))
(defn main [] i32
  (let [last (closure () void) (fn [] void)]
    (for [i 0 3]
      (set last (fn [] void (println i))))   ; kept past its pass of the loop
    (run last))
  0)
