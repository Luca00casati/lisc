; defop of a name: a function overloaded by its arguments' types; and generics
; given their types first, (new f32 4)
(import "std/mem")
(import "math")
(using math)

(struct v2 [x f32] [y f32])
(defop min [a f32] [b f32] f32 (if (< a b) a b))
(defop min [a v2] [b v2] v2 {v2 :x (min a.x b.x) :y (min a.y b.y)})
(defop min [a i32] [b i32] [c i32] i32 (min (min a b) c))   ; another arity
(defop min [a i32] [b i32] i32 (if (< a b) a b))
(defop (twice T) [x T] T (+ x x))                           ; generic too
(defop show [p v2] void (println "v2 " p.x " " p.y))
(defop show [s str] void (println "str " s))

(defn (zeros T) [n u64] (ptr T) (mem::new T n))
(defn (size T) [] u64 (sizeof T))

(defn main [] i32
  (println (min 2.5 1.5) " " (min 3 9) " " (min 7 2 5) " " (twice 21) " " (twice 1.25))
  (show (min {v2 :x 1.0 :y 9.0} {v2 :x 4.0 :y 2.0}))
  (show "pot")
  (let [p (mem::new f32 4)] [q (zeros i64 3)]
    (set p[3] 2.5)
    (println p[0] " " p[3] " " q[2] " " (size f80) " " (pow 2.0 10.0)))   ; math's pow stays itself
  0)
