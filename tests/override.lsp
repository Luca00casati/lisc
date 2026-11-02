; a program's own definition replaces a C import of the same name, and
; defop prefers the parameter kind of a literal (int or float)
(import "libc")
(using libc)
(import "math")
(using math)
(struct box (v f32) (n u32))
(defn floor ((b box)) box (make box :v (floorf b.v) :n b.n))   ; replaces math.h's floor
(defop * ((a box) (k f32)) box (make box :v (* a.v k) :n a.n))
(defop * ((a box) (k u32)) box (make box :v a.v :n (* a.n k)))
(defn main () i32
  (let ((b (floor (* (* (make box :v 1.75 :n 3) 2.0) 7))))
    (printf c"%g %u %g\n" (cast f64 b.v) b.n (floorf 2.5)))
  0)
