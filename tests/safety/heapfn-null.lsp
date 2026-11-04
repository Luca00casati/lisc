; abort: call of a null heap-fn
(struct button [on-click (heap-fn (i32) void)])
(defn main [] -> i32
  (let [b {button}]      ; on-click left zero
    (b.on-click 1))
  (return 0))
