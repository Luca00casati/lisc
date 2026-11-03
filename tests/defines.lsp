; -D NAME[=value] (given by tests/run.sh: -DWIDTH=640 -DNAME=teapot -DFAST), defined, if-defined
(import "libc")
(using libc)
(defn main [] i32
  (printf c"%d %s %d %d\n" WIDTH NAME (cast i32 (defined FAST)) (cast i32 (defined SLOW)))
  (if-defined SLOW (printf c"%d\n" SLOW-ONLY-NAME) (printf c"no SLOW\n"))
  0)
