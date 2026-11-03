; strings are UTF-8 and know their size (in bytes); no terminator
(import "libc")
(using libc)
(defn main [] i32
  (let [s "héllo €"] [e "\u{1F600} \u{e9}"]
    (printf c"%.*s (%lu bytes)\n%.*s (%lu bytes)\n" (cast i32 s.len) s.data s.len (cast i32 e.len) e.data e.len)
    (printf c"second byte of e: 0x%02x\n" e[1]))
  0)
