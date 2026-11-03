; error: 'scale' is private to module meta-a
(import "../mods/meta-a" :as a)
(defmacro m! (x) (a::scale x))
(defn main [] i32 (m! 1))
