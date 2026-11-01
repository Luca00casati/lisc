; -D NAME[=value] (given by tests/run.sh: -DWIDTH=640 -DNAME=teapot -DFAST), defined, if-defined
(c-include "stdio.h")
(defn main () i32
  (printf "%d %s %d %d\n" WIDTH NAME (cast i32 (defined FAST)) (cast i32 (defined SLOW)))
  (if-defined SLOW (printf "%d\n" SLOW-ONLY-NAME) (printf "no SLOW\n"))
  0)
