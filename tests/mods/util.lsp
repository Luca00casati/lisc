; small helpers meant for (import ... :all)
(defn twice ((x i32)) i32 (* 2 x))
(defn len ((s (ptr i8))) u64 (strlen s))     ; same name as geom's len: no clash, it's util::len
(c-include "string.h")
