; macros: a compile-time Lisp over the program's forms, with access to types
(import "libc")
(using libc)

(defmacro swap! (a b)                    ; gensym: a temporary that cannot clash
  (let ((tmp (gensym "tmp")))
    `(let ((,tmp ,a)) (set ,a ,b) (set ,b ,tmp))))

(defmacro unroll ((var n) &body body)     ; a loop expanded at compile time
  `(do ,@(map (lambda (i) `(let ((,var ,i)) ,@body)) (range n))))

(defmeta format-of (t)                   ; a compile-time helper function
  (cond ((eq t 'f32) "%g") ((eq t 'f64) "%g") ((eq t '(ptr i8)) "%s")
        ((eq (type-kind t) 'int) "%ld")
        (else (error "show: no format for " t))))

(defmacro show (x)                       ; dispatch on the argument's type
  (let ((t (type-of x)))
    `(printf ,(str (str x) " = " (format-of t) "\n")
             ,(cond ((eq t 'f32) `(cast f64 ,x))
                    ((eq (type-kind t) 'int) `(cast i64 ,x))
                    (else x)))))

(defn main () i32
  (let ((a 1) (b 2))
    (swap! a b)
    (printf "swap %d %d\n" a b))
  (let ((sum 0))
    (unroll (i 4) (set sum (+ sum (* i 10))))
    (printf "unrolled %d\n" sum))
  (let ((f f32 1.5) (n u8 200) (s "text"))
    (show f)
    (show (* f 2.0))
    (show n)
    (show s))
  0)
