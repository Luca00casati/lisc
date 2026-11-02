; error: T is f64 here but i32 in an earlier argument
(defn (biggest T) ((a T) (b T)) T (if (> a b) a b))
(defn main () i32 (let ((x i32 1) (y f64 2.0)) (biggest x y)) 0)
