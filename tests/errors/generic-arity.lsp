; error: pair takes 2 types, got 1
(struct (pair A B) [a A] [b B])
(defn main [] -> i32 (let [p {(pair i32)}] (return p.a)))
