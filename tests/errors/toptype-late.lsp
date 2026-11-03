; error: type-of in a top-level macro knows types and C declarations, not the program's functions, globals and constants
(def g i32 1)
(defmacro like! (name e) `(def ,name ,(type-of e) 0))
(like! h g)
(defn main () i32 0)
