; generic operators: one defop for (vec2 T) whatever T is
(import "libc")

(struct (vec2 T) [x T] [y T])
(defn (v2 T) [x T] [y T] -> (vec2 T) (return {(vec2 T) :x x :y y}))

(defop (+ T) [a (vec2 T)] [b (vec2 T)] -> (vec2 T) (return (v2 (+ a.x b.x) (+ a.y b.y))))
(defop (* T) [a (vec2 T)] [k T] -> (vec2 T) (return (v2 (* a.x k) (* a.y k))))
(defop (= T) [a (vec2 T)] [b (vec2 T)] -> bool (return (and (= a.x b.x) (= a.y b.y))))

; a fully generic one never takes over the numbers' own operators
(struct money [cents i64])
(defop (- T) [a T] [b T] -> T (return {money :cents 999}))

(defn main [] -> i32
  (let [a (v2 1.5 2.0)] [b (v2 0.25 0.5)] [i (v2 3 4)] [j (v2 10 20)]
    (let [c (* (+ a b b) 2.0)] [k (+ i j)]
      (libc::printf c"%g %g | %d %d | %d %d\n" c.x c.y k.x k.y (cast i32 (= k (v2 13 24))) (cast i32 (= i j))))
    (let [m (- {money :cents 5} {money :cents 2})]
      (libc::printf c"%d %ld\n" (- 7 2) m.cents)))
  (return 0))
