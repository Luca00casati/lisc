; str: a string with a size (bytes), not NUL-terminated; c"..." for C
(import "libc")
(using libc)

(struct named [label (ptr i8)] [id i32])          ; a C-style field: a literal is its pointer

(defn count-a [s str] u64                      ; strs pass by value (two registers)
  (let [n u64 0]
    (for [i u64 0 s.len] (when (= s[i] 97) (+= n 1)))   ; 'a'
    n))

(defn tail [s str] [from u64] str              ; a slice: no copy, no terminator needed
  {str :data (+ s.data from) :len (- s.len from)})

(defn main [] i32
  (let [s "banana split"] [t (tail s 7)] [z "a\0b"] [n {named :label c"teapot" :id 3}]
    (printf c"%.*s: %lu a's; tail \"%.*s\" (%lu bytes)\n" (cast i32 s.len) s.data (count-a s) (cast i32 t.len) t.data t.len)
    (printf c"a zero byte inside: len %lu, last 0x%02x\n" z.len z[2])
    (printf c"C sees: %s %d, %s\n" n.label n.id (if (> s.len 3) c"long" c"short"))
    (printf c"sizeof str %lu\n" (sizeof str)))
  0)
