; operator overloading by static type, and a macro that writes defops for any struct
(import "libc")
(using libc)

(struct v3 [x f32] [y f32] [z f32])
(struct rgb [r f32] [g f32] [b f32])

; generate componentwise + - * and = for a struct, by asking for its fields
(defmacro derive-ops! (T)
  (let [fields (map first (type-fields T))]
    `(do ,@(map (lambda (op)
                  `(defop ,op [a ,T] [b ,T] ,T
                     {,T ,@(mapcat (lambda (f) `(,(sym c":" f) (,op (. a ,f) (. b ,f)))) fields)}))
                '(+ - *))
         (defop = [a ,T] [b ,T] bool
           (and ,@(map (lambda (f) `(= (. a ,f) (. b ,f))) fields))))))

(derive-ops! v3)
(derive-ops! rgb)

; by hand: scalar times vector (either order), negation, dot product
(defop * [a v3] [k f32] v3 {v3 :x (* a.x k) :y (* a.y k) :z (* a.z k)})
(defop * [k f32] [a v3] v3 (* a k))
(defop - [a v3] v3 {v3 :x (- a.x) :y (- a.y) :z (- a.z)})
(defn dot [a v3] [b v3] f32 (+ (* a.x b.x) (* a.y b.y) (* a.z b.z)))

(defn show [name (ptr i8)] [v v3] void
  (printf c"%s %g %g %g\n" name (cast f64 v.x) (cast f64 v.y) (cast f64 v.z)))

(defn main [] i32
  (let [a {v3 :x 1.0 :y 2.0 :z 3.0}]
       [b {v3 :x 0.5 :y 0.5 :z 0.5}]
    (show c"a+b" (+ a b))
    (show c"a+b+b" (+ a b b))             ; n-ary: folds left
    (show c"a-b" (- a b))
    (show c"a*b" (* a b))
    (show c"2a" (* 2.0 a))                ; the literal takes the f32 the definition wants
    (show c"a*3" (* a 3))
    (show c"-a" (- a))
    (show c"mixed" (- (* 2.0 (+ a b)) a))
    (printf c"dot %g  a=a %d  a=b %d\n" (cast f64 (dot a b)) (cast i32 (= a a)) (cast i32 (= a b)))
    ; built-in operators still work on numbers
    (printf c"numbers %d %g\n" (+ 1 2 3) (cast f64 (* a.x 4.0))))
  (let [c (+ {rgb :r 0.25 :g 0.5 :b 1.0} {rgb :r 0.25 :g 0.25 :b 0.0})]
    (printf c"rgb %g %g %g\n" (cast f64 c.r) (cast f64 c.g) (cast f64 c.b)))
  0)
