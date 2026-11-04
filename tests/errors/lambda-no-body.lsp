; error: a lambda giving i32 needs a body ending in (return e), or write the result itself
(defn main [] -> i32
  (let [f (lambda [x i32] -> i32)]
    (return (f 1))))
