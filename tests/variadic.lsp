; [xs ..T]: the rest of the arguments, as a (slice T); (f ..s) or (f .. (expr)) passes a slice on
(defn sum [xs ..i32] -> i32
  (let [s 0]
    (for [x :in xs] (+= s x))
    (return s)))
(defn label [name str] [parts .. str]
  (print name ":")
  (for [p :in parts] (print " " p))
  (println))
(defn (biggest T) [first T] [rest ..T] -> T
  (let [m first]
    (for [x :in rest] (when (> x m) (set m x)))
    (return m)))
(defn total [xs ..i32] -> i32 (return (+ (sum ..xs) 1000)))   ; passing the rest on
(defn main [] -> i32
  (let [a {(array i32 3) 7 8 9}]
    (println (sum) " " (sum 1) " " (sum 1 2 3) " " (sum ..(slice-of a)) " " (total 1 2))
    (label "parts" "lid" "spout" "handle")
    (label "none")
    (println (biggest 3 9 4) " " (biggest 2.5 1.0)))
  (return 0))
