; modules: qualified names (one level), aliases, using, private names, macros across modules
(import "libc")
(using libc)
(import "mods/geom")                  ; geom::name
(import "mods/geom" :as g)            ; the same module, also as g::name
(import "mods/util")
(using util)                          ; util's names unqualified too

(defn main () i32
  (let ((a (geom::v 3.0 4.0)) (b (g::v 1.0 geom::UNIT)) (c geom::v2 (+ a b)))
    (printf "len %g  sum %g %g\n" (cast f64 (geom::len a)) (cast f64 c.x) (cast f64 c.y))
    (let ((d (geom::doubled b)) (e (g::doubled a)))     ; the macro reaches geom's private helper
      (printf "doubled %g %g, %g  calls %d\n" (cast f64 d.x) (cast f64 d.y) (cast f64 e.x) (geom::call-count))))
  (printf "twice %d  strlen %lu\n" (twice 21) (len "lisc"))   ; util's, via using
  (let ((len 5))                      ; a local named like an imported function
    (printf "local %d, util::len %lu\n" len (util::len "ab")))
  0)
