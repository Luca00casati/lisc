; -D NAME[=value] (given by tests/run.sh: -DWIDTH=640 -DNAME=teapot -DFAST), defined, if-defined
(import "libc")
(using libc)

; at the top level, if-defined chooses imports and definitions (like #ifdef)
(if-defined FAST
  (do (import "math")
      (defn speed [] -> i32 (return 2)))
  (defn speed [] -> i32 (return 1)))
(if-defined SLOW (import "no/such/module"))   ; not chosen: never loaded

(defn main [] -> i32
  (printf c"%d %s %d %d\n" WIDTH NAME (cast i32 (defined FAST)) (cast i32 (defined SLOW)))
  (if-defined SLOW (printf c"%d\n" SLOW-ONLY-NAME) (printf c"no SLOW\n"))
  (printf c"speed %d, sqrt %.1f\n" (speed) (math::sqrt 16.0))
  (return 0))
