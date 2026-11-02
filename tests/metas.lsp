; compile-time helpers (defmeta) belong to their module, like other names
(import "libc")
(import "mods/meta-a" :as a)
(import "mods/meta-b" :as b)
(defmeta scale (x) (+ x 1))                     ; this module's own
(defmacro plus-one (x) (scale x))
(defmacro loud (s) (cstr (a::shout s)))         ; another module's helper, by its full name
(defn main () i32
  (libc::printf c"%d %d %d %s\n" (a::tens 4) (b::hundreds 4) (plus-one 4) (loud "hi"))
  0)
