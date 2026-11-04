; :optnone functions are compiled apart (as at -O0) and linked with the rest:
; globals, strings, structs and calls across the two must all line up
(struct pair [a i64] [b i64] [c (array f32 3)])
(def total i64 0)
(private
  (def hidden i32 7)
  (defn add [p pair] -> pair (return {pair :a (+ p.a 1) :b (* p.b 2) :c p.c}))   ; optimised, called from both
  (defn record [x i64] (+= total x)))

(defn build [n i32] -> pair :optnone
  (let [p {pair :a 1 :b 1 :c {(array f32 3) 0.5 1.5 2.5}}] [q pair]
    (for [_i 0 n] (set p (add p)))
    (set q p)                                  ; whole-struct copies
    (record q.a)
    (+= hidden 1)
    (println "built " q.a " " q.b " " q.c)
    (return q)))

(defn twice [p pair] -> i64 (return (* 2 (+ p.a p.b))))      ; optimised, calling the optnone one below
(defn (show T) [x T] :optnone (println "show " x))

(defn main [] -> i32
  (let [p (build 5)] [f (fn [x i64] -> i64 :optnone (return (+ x hidden)))]
    (record (twice p))
    (show p.b) (show 2.5)
    (println "total " total " hidden " hidden " fn " (f 1)))
  (return 0))
