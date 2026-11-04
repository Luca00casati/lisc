; any: a value and its type's description (type-info), for functions of any argument types
(struct pt [x i32] [y f32])
(enum color u8 red green (blue 7))
(variant shape (circle [r f32]) none)
(defn describe [a any]
  (let [t a.type]
    (print t.name " kind " t.kind " size " t.size)
    (when (= t.kind type-kind.int) (print " = " (deref (cast (ptr i32) a.data))))
    (when (= t.kind type-kind.struct)
      (for [f :in t.fields] (print " :" f.name " " f.type.name "@" f.offset)))
    (when (= t.kind type-kind.enum)
      (for [n :in t.names] (print " " n)))
    (when (= t.kind type-kind.variant)
      (for [n :in t.names] (print " " n)))
    (when (= t.kind type-kind.ptr) (print " to " t.elem.name))
    (println)))
(defn all [xs ..any] (for [x :in xs] (describe x)))
(defn main [] -> i32
  (all 42 2.5 "hi" true {pt :x 1 :y 2.0} color.green (shape.circle 1.0) (addr main) {(array u8 3) 1 2 3})
  (println (= (type-info-of i32) (type-info-of i32)) " " (= (type-info-of i32) (type-info-of u32)))
  (return 0))
