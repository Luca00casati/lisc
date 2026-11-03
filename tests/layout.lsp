; struct options :packed and :align, export to C
(import "libc")
(using libc)
(struct plain [a u8] [b u32] [c u16])
(struct hdr :packed [a u8] [b u32] [c u16])
(struct line :align 64 [v f32])
(defn twice [x i32] i32 (* x 2))
(export twice)                                  ; called by name from layout.c
(extern call_twice (i32) i32)

(defn main [] i32
  (let [h {hdr :a 1 :b 0x11223344 :c 7}]
    (printf c"plain %lu  packed %lu (b at byte %ld)  line %lu align %lu\n" (sizeof plain) (sizeof hdr)
            (- (cast i64 (addr h.b)) (cast i64 (addr h))) (sizeof line) (alignof line))
    (printf c"h.b %x  C calls twice: %d\n" h.b (call_twice 21)))
  0)
