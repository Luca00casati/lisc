; std/vec — growable arrays. (vec::define name T) makes a struct `name`
; (data (ptr T), len, cap) and the functions name-push, name-pop, name-clear,
; name-free, name-slice; (aref v.data i) reaches an element.
(import "libc")

(defmacro define (name T)
  (let ((push (sym name "-push")) (pop (sym name "-pop")) (clear (sym name "-clear"))
        (free (sym name "-free")) (slice (sym name "-slice")))
    `(do
       (struct ,name (data (ptr ,T)) (len u64) (cap u64))
       (defn ,push ((v (ptr ,name)) (x ,T)) void
         (when (= v.len v.cap)
           (set v.cap (if (= v.cap 0) 8 (* v.cap 2)))
           (set v.data (cast (ptr ,T) (libc::realloc v.data (* v.cap (sizeof ,T)))))
           (when (= v.data null) (libc::abort)))
         (set (aref v.data v.len) x)
         (+= v.len 1))
       (defn ,pop ((v (ptr ,name))) ,T
         (-= v.len 1)
         (aref v.data v.len))
       (defn ,clear ((v (ptr ,name))) void (set v.len 0))
       (defn ,free ((v (ptr ,name))) void
         (libc::free v.data)
         (set (deref v) null))
       (defn ,slice ((v (ptr ,name))) (slice ,T)
         (make (slice ,T) :data v.data :len v.len)))))
