; std/mem — typed allocation (zeroed, as everything in lisc starts)
(import "libc")

; (mem::new T n): n zeroed T's, a (ptr T); aborts if there is no memory
(defmacro new (T n)
  `(cast (ptr ,T) (zeroed (* (sizeof ,T) (cast u64 ,n)))))

(defn zeroed [bytes u64] (ptr void)
  (let [p (libc::calloc 1 bytes)]
    (when (and (= p null) (> bytes 0))
      (libc::fputs c"out of memory\n" libc::stderr)
      (libc::abort))
    p))

(defn free [p (ptr void)] void (libc::free p))
