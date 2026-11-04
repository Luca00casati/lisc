; error: lisc: 3 errors
(defn a [] -> i32 (return (+ 1 nope)))
(defn b [] -> i32 (let [x i32 1.5] (return x)))
(defn ok [] -> i32 (return 7))
(defn c [] -> i32 (return (undefined-fn 2)))
(defn main [] -> i32 (return (ok)))
