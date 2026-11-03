; error: a closure can't be behind a pointer (here (closure () void))
; note: errors/closure-generic.lsp:5:17: note: in (vec (closure () void)), used here
(import "std/vec")
(defn main [] i32
  (let [k 1] [v (vec::vec (closure () void))]
    (vec::push v (fn [] void (println k))))
  0)
