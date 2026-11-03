; error: a fn can't use 'k', a local of the code around it (it has no captures)
(defn main () i32
  (let [k 3] [f (fn [x i32] i32 (+ x k))] (f 1)))
