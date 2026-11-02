; strings are UTF-8: written directly or with \u{...}; strlen counts bytes
(import "libc")
(using libc)
(defn main () i32
  (let ((s "héllo €") (e "\u{1F600} \u{e9}"))
    (printf "%s (%lu bytes)\n%s (%lu bytes)\n" s (strlen s) e (strlen e)))
  0)
