; C headers through libclang: functions, types, struct layouts, enums, #defines
(import "libc")
(using libc)
(import "math")
(using math)

(defn main [] -> i32
  ; printf's real declaration: variadic, const char *
  (printf c"pi %.6f  sqrtf(2) %.7f  INT_MAX %d  UINT32_MAX %u\n" M_PI (cast f64 (sqrtf 2.0)) INT_MAX UINT32_MAX)
  ; malloc / strlen / memcpy / free with C's types (size_t is u64)
  (let [s "lisc talks to libc"] [n s.len] [buf (cast (ptr i8) (malloc (+ n 1)))]   ; a str knows its size
    (memcpy buf s.data n)
    (set buf[n] 0)                        ; C wants a terminator: add it
    (set buf[0] 76)                       ; 'L'
    (printf c"%s (%lu bytes)\n" buf n)
    (free buf))
  ; a C struct with clang's layout, and typedef'd field types
  (let [ts {timespec :tv_sec 3 :tv_nsec 500}]
    (printf c"timespec %lu bytes: %ld.%09ld\n" (sizeof timespec) ts.tv_sec ts.tv_nsec))
  (let [x uint32_t 0xdeadbeef] [y int64_t -1]
    (printf c"%x %ld %lu\n" x y (sizeof uint8_t)))
  ; stderr is an extern variable of type (ptr FILE)
  (fprintf stderr c"")
  (printf c"EXIT_FAILURE %d  SEEK_END %d  EOF %d\n" EXIT_FAILURE SEEK_END EOF)
  (return EXIT_SUCCESS))
