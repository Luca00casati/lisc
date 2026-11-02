; std/vec — growable arrays: (vec T), with push, pop, clear, items (a slice
; over them) and release. (aref v.data i) reaches an element.
;
;   (import "std/vec") (using vec)
;   (let ((v (make (vec i32))))
;     (push (addr v) 42)
;     (release (addr v)))
(import "libc")

(struct (vec T) (data (ptr T)) (len u64) (cap u64))

(defn (push T) ((v (ptr (vec T))) (x T)) void
  (when (= v.len v.cap)
    (set v.cap (if (= v.cap 0) 8 (* v.cap 2)))
    (set v.data (cast (ptr T) (libc::realloc v.data (* v.cap (sizeof T)))))
    (when (= v.data null) (libc::abort)))
  (set (aref v.data v.len) x)
  (+= v.len 1))

; the last element, taken off (the vec must not be empty)
(defn (pop T) ((v (ptr (vec T)))) T
  (-= v.len 1)
  (aref v.data v.len))

(defn (clear T) ((v (ptr (vec T)))) void (set v.len 0))

(defn (items T) ((v (ptr (vec T)))) (slice T)
  (make (slice T) :data v.data :len v.len))

; free the elements' memory; the vec is empty again
(defn (release T) ((v (ptr (vec T)))) void
  (libc::free v.data)
  (set (deref v) null))
