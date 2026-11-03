; variant: tagged unions, built with name.case and taken apart with match
(import "std/option" :as opt)
(using opt)

(variant shape (circle [r f32]) (rect [w f32] [h f32]) none)
(variant (lst T) (cons [head T] [tail (ptr (lst T))]) nil)
(defmacro two-cases! (name a b) `(variant ,name ,a ,b))
(two-cases! dir up down)

(defn area [s shape] f32
  (match s
    ((circle r) (* 3.0 r r))
    ((rect w h) (* w h))
    (none 0.0)))

(defn (len T) [l (lst T)] i32
  (match l ((cons _ t) (+ 1 (len (deref t)))) (nil 0)))

(defn find [xs (slice i32)] [want i32] (option u64)
  (for [i x :in xs] (when (= x want) (return (option.some i))))
  option.none)

(defn parse [s str] (result i32 str)
  (if (= s.len 0) (result.err "empty") (result.ok (cast i32 s.len))))

(defn main [] i32
  (let [shapes {(array shape 3) (shape.circle 1.0) (shape.rect 2.0 3.0) shape.none}] [total f32 0.0]
       [a {(array i32 4) 5 30 8 1}]
    (for [s :in shapes] (+= total (area s)))
    (println shapes " total " total)
    (println (find (slice-of a) 8) " " (find (slice-of a) 7) " " (parse "") " " (parse "abc"))
    (match (find (slice-of a) 30)
      ((some i) (println "found at " i))
      (else (println "not there")))
    (println (or-else (find (slice-of a) 1) 99) " " (or-else (find (slice-of a) 2) 99) " " (is-some (find (slice-of a) 5)) " " (is-ok (parse "")))
    (let [s (shape.circle 2.0)] [o (option i32) option.none] [p (ptr shape) (addr s)]
      (match p ((circle r) (set r 5.0)) (else))   ; r is the field itself (and p is followed)
      (println s " " (area s) " " o " " (sizeof shape) " " s.tag))
    (let [c (lst i32) lst.nil] [b (lst.cons 2 (addr c))] [x (lst.cons 1 (addr b))]
      (println (len x) " " dir.down " " (match dir.up (up "up") (down "down"))))
    (println (match (parse "xy") ((ok n) (* n 10)) ((err _) -1))))
  0)
