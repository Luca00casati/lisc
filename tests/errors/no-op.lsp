; error: no (+ v2 v2): define it with defop
(struct v2 (x f32) (y f32))
(defn main () i32
  (let ((a (make v2 :x 1.0)))
    (+ a a)
    0))
