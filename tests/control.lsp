; case (a jump table), cond, defer, character literals
(import "libc")
(using libc)

(enum op u8 MOV ADD MUL (CMP 17) JMP)

(defn name [o op] (ptr i8)
  (case o
    (op.MOV c"mov")
    ((op.ADD op.MUL) c"alu")
    (op.CMP c"cmp")
    (else c"other")))

(defn classify [c u8] i32
  (cond ((and (>= c #\0) (<= c #\9)) 1)
        ((= c #\space) 2)
        (else 0)))

(def log (array u8 32))
(def nlog u32 0)
(defn note [c u8] void (set log[nlog] c) (+= nlog 1))

; defers run at every exit, innermost first, after the result is computed
(defn early [x i32] i32
  (defer (note #\a))
  (let [y (* x 2)]
    (defer (note #\b))
    (when (> y 10) (return y))
    (defer (note #\c))
    (+ y 1)))

(defn loop-defers [] void
  (for [i 0 4]
    (defer (note #\l))
    (when (= i 1) (continue))
    (when (= i 2) (break))
    (note #\.)))

(defn main [] i32
  (printf c"%s %s %s %s %s  (op.CMP = %d, size %lu)\n" (name op.MOV) (name op.MUL) (name op.CMP) (name op.JMP)
          (name (cast op 99)) (cast i32 op.CMP) (sizeof op))
  (printf c"%d %d %d  #\\é = %d\n" (classify #\7) (classify #\space) (classify #\x) #\é)
  (printf c"%d " (early 3)) (printf c"%d\n" (early 8))
  (loop-defers)
  (printf c"log %.*s\n" (cast i32 nlog) (addr log[0]))
  (let [n (case 3 (1 10) (3 30) (else 0))] (printf c"case value %d\n" n))
  0)
