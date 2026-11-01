; constant initializers for globals: nested arrays and structs
(import "libc")
(using libc)
(struct pt (x f32) (y f32) (tag u8))
(def table (array (array i32 3) 2) (make (array (array i32 3) 2) (make (array i32 3) 1 2 3) (make (array i32 3) 4 5)))
(def origin pt (make pt :y 2.5 :tag 7))
(def pts (array pt 2) (make (array pt 2) (make pt :x 1.0) (make pt :x 3.0 :y 4.0)))
(defn main () i32
  (printf "%d %d %d %d\n" (aref (aref table 0) 2) (aref (aref table 1) 0) (aref (aref table 1) 1) (aref (aref table 1) 2))
  (printf "%g %g %d %g\n" (cast f64 origin.x) (cast f64 origin.y) origin.tag (cast f64 (. (aref pts 1) y)))
  0)
