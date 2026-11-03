; print / println: each value as its type shows it
(import "libc")
(enum color u8 red green blue)
(struct point [x i32] [y i32])
(struct named [name str] [p point] [c color] [tags (array u8 3)] [ok bool :bits 1])

(defn main [] i32
  (let [n named]
    (set n {named :name "kettle" :p {point :x 1 :y -2} :c color.blue :tags {(array u8 3) 7 8 9} :ok true})
    (println n)
    (println "ints " -5 " " (cast u8 200) " " (cast i64 -9000000000) " " (cast u64 18000000000000000000) " " #\a)
    (println "floats " 0.1 " " (cast f32 0.1) " " 1e300 " " (cast f80 2.5))
    (println "text " c"a C string" " " "50% " (slice-of "abcdef" 1 3) " " color.green " " false)
    (print "no newline;")
    (print " then " (slice-of {(array f32 2) 0.5 1.5}) "\n")
    (println :to libc::stdout "to " "stdout"))
  0)
