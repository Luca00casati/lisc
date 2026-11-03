; type-of in top-level macros: a function per field of a struct, its result
; type taken from an expression's type
(import "libc")

(struct point (x f32) (y f32) (tag u8))

; (getters point p): (defn p-x ((p point)) f32 p.x) ... from the struct's fields
(defmacro getters! (T prefix)
  `(do ,@(map (lambda (fd)
                `(defn ,(sym prefix "-" (first fd)) ((p ,T)) ,(nth fd 1) (. p ,(first fd))))
              (type-fields T))))
(getters! point pt)

; a constant whose type comes from a C function's result
(defmacro def-like! (name expr init) `(def ,name ,(type-of expr) ,init))
(def-like! len-total (libc::strlen c"") 7)
(def-like! half (* 0.5 (. (make point) x)) 1.25)

(defmacro size-of-value! (e) `(sizeof ,(type-of e)))

(defn main () i32
  (let ((p (make point :x 1.5 :y 2.5 :tag 9)))
    (libc::printf c"%g %g %d | %lu %lu %g %lu\n" (cast f64 (pt-x p)) (cast f64 (pt-y p)) (cast i32 (pt-tag p))
                  len-total (size-of-value! len-total) (cast f64 half) (size-of-value! half)))
  0)
