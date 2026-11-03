; the built-in C modules: libc, math, posix, io, signal, time, thread
(import "libc") (import "math") (import "posix") (import "io") (import "signal") (import "time") (import "thread")
(using libc posix)                        ; size_t, NULL ... come from both: one name, not ambiguous

(defn worker [arg (ptr void)] (ptr void)
  (set (deref (cast (ptr i32) arg)) 42)
  null)

(defn main [] i32
  (let [n size_t (strlen c"split")] [fd (open c"/dev/null" O_RDWR)]
       [p {io::pollfd :fd fd :events io::POLLOUT}]
       [ts {time::timespec}] [th thread::pthread_t 0] [out i32 0]
    (printf c"libc %lu  math %g\n" n (math::floor 2.5))
    (printf c"posix fd ok %d  io poll %d\n" (cast i32 (>= fd 0)) (io::poll (addr p) 1 0))
    (time::clock_gettime time::CLOCK_MONOTONIC (addr ts))
    (printf c"time ok %d  signal %d\n" (cast i32 (> ts.tv_sec 0)) signal::SIGINT)
    (thread::pthread_create (addr th) null worker (addr out))
    (thread::pthread_join th null)
    (printf c"thread %d\n" out)
    (close fd))
  0)
