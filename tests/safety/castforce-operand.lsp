; abort: division by zero
(defn main [] -> i32
  (let [x i32 300]
    (println "printed before the abort")
    (println (cast-force u8 (/ x 0))))     ; only the cast itself is unchecked
  (return 0))
