; std/str — working with str (UTF-8 bytes with a length, no terminator)
(import "libc")

; the same bytes?
(defn eq ((a str) (b str)) bool
  (and (= a.len b.len) (= (libc::memcmp a.data b.data a.len) 0)))

(defn starts-with ((s str) (prefix str)) bool
  (and (>= s.len prefix.len) (= (libc::memcmp s.data prefix.data prefix.len) 0)))

(defn ends-with ((s str) (suffix str)) bool
  (and (>= s.len suffix.len) (= (libc::memcmp (+ s.data (- s.len suffix.len)) suffix.data suffix.len) 0)))

; the index of the first `byte` in s, or s.len if there is none
(defn find ((s str) (byte u8)) u64
  (let ((i u64 0))
    (while (and (< i s.len) (!= (aref s i) byte)) (+= i 1))
    i))

; a str over a C string's bytes (no copy)
(defn from-c ((p (ptr i8))) str
  (make str :data (cast (ptr u8) p) :len (libc::strlen p)))

; s as a C string, in buf (cap bytes, at least s.len + 1); null if it doesn't fit
(defn to-c ((s str) (buf (ptr i8)) (cap u64)) (ptr i8)
  (when (>= s.len cap) (return null))
  (libc::memcpy buf s.data s.len)
  (set (aref buf s.len) 0)
  buf)

; write s to stdout (or to a C FILE)
(defn print ((s str)) void (libc::fwrite s.data 1 s.len libc::stdout))
(defn fprint ((f (ptr libc::FILE)) (s str)) void (libc::fwrite s.data 1 s.len f))
