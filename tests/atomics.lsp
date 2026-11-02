; atomics: four threads count without losing updates; swap, cas, load/store, fence
(import "libc") (import "thread")
(using libc)
(def counter u64 0)
(def flag i32 0)

(defn work ((arg (ptr void))) (ptr void)
  (for (_i 0 100000) (atomic-add counter 1 :relaxed))
  null)

(defn main () i32
  (let ((ts (array thread::pthread_t 4)))
    (for (k 0 4) (thread::pthread_create (addr (aref ts k)) null work null))
    (for (k 0 4) (thread::pthread_join (aref ts k) null)))
  (printf c"counter %lu\n" (atomic-load counter))
  (atomic-store flag 1 :release)
  (let ((old (atomic-swap flag 7)) (won (atomic-cas flag 7 9)) (lost (atomic-cas flag 7 11)))
    (fence)
    (printf c"swap old %d  cas %d %d  now %d\n" old (cast i32 won) (cast i32 lost) (atomic-load flag :acquire)))
  0)
