; error: a closure can't be set again
(defn run [f (closure () void)] (f))
(defn main [] -> i32
  (let [last (closure () void) (fn [])]
    (for [i 0 3]
      (set last (fn [] (println i))))   ; kept past its pass of the loop
    (run last))
  (return 0))
