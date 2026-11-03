; error: lisc: 3 errors
(defn a [] i32 (+ 1 nope))
(defn b [] i32 (let [x i32 1.5] x))
(defn ok [] i32 7)
(defn c [] i32 (undefined-fn 2))
(defn main [] i32 (ok))
