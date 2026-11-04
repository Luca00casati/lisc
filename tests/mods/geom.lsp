; a module: a struct, an operator, functions, a macro and private helpers
(import "math")
(using math)

(struct v2 [x f32] [y f32])
(defop + [a v2] [b v2] -> v2 (return {v2 :x (+ a.x b.x) :y (+ a.y b.y)}))
(defn v [x f32] [y f32] -> v2 (return {v2 :x x :y y}))
(defn len [a v2] -> f32 (return (sqrtf (+ (* a.x a.x) (* a.y a.y)))))
(const UNIT 1.0)

(private
  (defn scale-by [a v2] [k f32] -> v2 (return {v2 :x (* a.x k) :y (* a.y k)})))   ; private
(private
  (def calls i32 0))

; uses the private helper and counter: expands correctly in any module
(defmacro doubled! (e)
  `(do (set calls (+ calls 1)) (scale-by ,e 2.0)))
(defn call-count [] -> i32 (return calls))

; a private generic (tests/errors/generic-private.lsp)
(private
  (defn (hidden T) [x T] -> T (return x)))
