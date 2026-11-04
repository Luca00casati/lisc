; std/fmt: {} placeholders filled with values of any type
(import "std/fmt")
(struct pt [x i32] [y f32])
(enum color u8 red green (blue 7))
(variant shape (circle [r f32]) (rect [w f32] [h f32]) none)
(defn main [] -> i32
  (fmt::println "hello {}, {} + {} = {}" "world" 2 3 (+ 2 3))
  (fmt::println "hex {:x} {:X} {:08x} bin {:b} oct {:o} neg {:x}" 255 255 48879 5 8 -1)
  (fmt::println "floats {} {:.2} {:e} {:8.3}|" 3.14159 3.14159 31415.9 2.5)
  (fmt::println "pad [{:6}] [{:<6}] [{:^6}] [{:*^7}] [{:06}] [{:>5}]" 42 42 42 "ab" -42 "x")
  (fmt::println "bool {} char-ish {} ptr-null {} cstr {} {:?}" true #\a (cast (ptr u8) null) c"text" c"quoted")
  (fmt::println "struct {} enum {} {}" {pt :x 1 :y 2.5} color.green color.blue)
  (fmt::println "variant {} {} {}" (shape.circle 2.0) (shape.rect 1.0 2.0) shape.none)
  (let [a {(array i32 3) 1 2 3}]
    (fmt::println "array {} slice {} strs {}" a (slice-of a 1) {(array str 2) "a" "b"}))
  (fmt::println "u64 {} i64 {} u8 {} i8 {} i128 {}" (cast u64 18446744073709551615) (cast i64 -9223372036854775807) (cast u8 255) (cast i8 -128) (* (cast i128 -1000000000000) 1000000000000))
  (fmt::println "braces {{}} missing {} {}" 1)
  (let [s (fmt::format "{}-{}" "a" 7)]
    (fmt::println "format gave [{}] len {}" s s.len)
    (fmt::release-str s))
  (let [b {fmt::buffer}]
    (for [i 0 3] (fmt::append (addr b) "{} " i))
    (fmt::println "buffer [{}]" (fmt::text b))
    (fmt::release (addr b)))
  (return 0))
