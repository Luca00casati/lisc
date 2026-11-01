; (include ...) reads other files' forms in place; each file once
(c-include "stdio.h")
(include "inc/lib.lsp" "inc/more.lsp")
(defn main () i32
  (let ((n 20))
    (inc! n)
    (printf "%d\n" (twice n)))
  0)
