; (include ...) reads other files' forms in place; each file once
(import "libc")
(using libc)
(include "inc/lib.lsp" "inc/more.lsp")
(defn main [] -> i32
  (let [n 20]
    (inc! n)
    (printf c"%d\n" (twice n)))
  (return 0))
