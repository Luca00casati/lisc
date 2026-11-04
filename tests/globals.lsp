; constant initializers for globals: nested arrays and structs
(import "libc")
(using libc)
(struct pt [x f32] [y f32] [tag u8])
(def table (array (array i32 3) 2) {(array (array i32 3) 2) {(array i32 3) 1 2 3} {(array i32 3) 4 5}})
(def origin pt {pt :y 2.5 :tag 7})
(def pts (array pt 2) {(array pt 2) {pt :x 1.0} {pt :x 3.0 :y 4.0}})
(defn main [] -> i32
  (printf c"%d %d %d %d\n" table[0][2] table[1][0] table[1][1] table[1][2])
  (printf c"%g %g %d %g\n" (cast f64 origin.x) (cast f64 origin.y) origin.tag (cast f64 (. pts[1] y)))
  (return 0))
