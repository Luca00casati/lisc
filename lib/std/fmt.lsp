; std/fmt — text with {} placeholders, filled with values of any type.
;
;   (format "x = {} at {:x}" x addr)   a str with its own memory: (release-str s) frees it
;   (print "..." args) (println ...)   to stdout; (eprint ...) (eprintln ...) to stderr
;   (write file "..." args)            to a C FILE
;   (append (addr buf) "..." args)     onto a buffer: (text buf) is a str of it so far;
;                                      (clear (addr buf)) empties it, (release (addr buf)) frees it
;
; {} shows a value as print does: numbers, bools, strs, C strings, pointers,
; arrays, slices and simds [1 2 3], structs {point :x 1 :y 2}, enums
; color.red, variants (shape.circle 2) and shape.none. A spec after a colon:
;
;   {:[fill][<^>][0][width][.precision][type]}
;   <  >  ^      left, right, centred in width (numbers go right, the rest left)
;   0            zeros before a number, after its sign
;   .N           a float with N decimals; a str cut to N bytes
;   x X b o      an integer in hex (lower, upper), binary, octal
;   e            a float in exponent form
;   ?            strs and C strings in quotes (as inside a struct)
;
; {{ and }} are braces. A {} without an argument shows {?}; arguments left
; over are ignored.
(import "libc")

(struct buffer [data (ptr u8)] [len u64] [cap u64])

; the text so far (valid until the buffer changes)
(defn text [b buffer] -> str (return {str :data b.data :len b.len}))
(defn clear [b (ptr buffer)] (set b.len 0))
(defn release [b (ptr buffer)]
  (libc::free b.data)
  (set (deref b) null))
(defn release-str [s str] (libc::free s.data))

(defn push-bytes [b (ptr buffer)] [p (ptr u8)] [n u64]
  (when (> (+ b.len n) b.cap)
    (let [cap (if (= b.cap 0) 64 b.cap)]
      (while (> (+ b.len n) cap) (set cap (* cap 2)))
      (set b.data (cast (ptr u8) (libc::realloc b.data cap)))
      (when (= b.data null) (libc::abort))
      (set b.cap cap)))
  (libc::memcpy (+ b.data b.len) p n)
  (+= b.len n))
(defn push-str [b (ptr buffer)] [s str] (push-bytes b s.data s.len))
(defn push-byte [b (ptr buffer)] [c u8]
  (let [one (array u8 1)]
    (set one[0] c)
    (push-bytes b (addr one[0]) 1)))

(defn append [b (ptr buffer)] [f str] [args ..any]
  (let [next u64 0] [i u64 0]
    (while (< i f.len)
      (let [c f.data[i]]
        (cond ((and (= c #\{) (< (+ i 1) f.len) (= f.data[(+ i 1)] #\{)) (push-byte b #\{) (+= i 2))
              ((and (= c #\}) (< (+ i 1) f.len) (= f.data[(+ i 1)] #\})) (push-byte b #\}) (+= i 2))
              ((= c #\{)
               (let [end i]
                 (while (and (< end f.len) (!= f.data[end] #\})) (+= end 1))
                 (if (>= end f.len)
                     (do (push-bytes b (+ f.data i) (- f.len i)) (set i f.len))   ; no closing brace: as it is
                     (let [sp (parse-spec (slice-of f (+ i 1) end))]
                       (if (< next args.len)
                           (show-padded b args[next] (addr sp))
                           (push-str b "{?}"))
                       (+= next 1)
                       (set i (+ end 1))))))
              (else (push-byte b c) (+= i 1)))))))

(defn format [f str] [args ..any] -> str
  (let [b {buffer}]
    (append (addr b) f ..args)
    (return (text b))))

(defn write [file (ptr libc::FILE)] [f str] [args ..any]
  (let [b {buffer}]
    (append (addr b) f ..args)
    (libc::fwrite b.data 1 b.len file)
    (release (addr b))))
(defn print [f str] [args ..any] (write libc::stdout f ..args))
(defn eprint [f str] [args ..any] (write libc::stderr f ..args))
(defn println [f str] [args ..any]
  (write libc::stdout f ..args)
  (libc::fputc 10 libc::stdout))
(defn eprintln [f str] [args ..any]
  (write libc::stderr f ..args)
  (libc::fputc 10 libc::stderr))

(private
  (struct spec [fill u8] [align u8] [zero bool] [width u64] [precision i64] [kind u8])

  ; ":[fill][<^>][0][width][.precision][type]" (or nothing)
  (defn parse-spec [s str] -> spec
    (let [sp {spec :fill #\space :precision -1}] [i u64 0]
      (when (or (= s.len 0) (!= s.data[0] #\:)) (return sp))
      (set i 1)
      (let [aligns "<^>"]
        (when (and (< (+ i 1) s.len) (has aligns s.data[(+ i 1)]))   ; a fill, then an alignment
          (set sp.fill s.data[i])
          (set sp.align s.data[(+ i 1)])
          (+= i 2))
        (when (and (< i s.len) (has aligns s.data[i]))
          (set sp.align s.data[i])
          (+= i 1)))
      (when (and (< i s.len) (= s.data[i] #\0))
        (set sp.zero true)
        (+= i 1))
      (while (and (< i s.len) (digit s.data[i]))
        (set sp.width (+ (* sp.width 10) (- s.data[i] #\0)))
        (+= i 1))
      (when (and (< i s.len) (= s.data[i] #\.))
        (+= i 1)
        (set sp.precision 0)
        (while (and (< i s.len) (digit s.data[i]))
          (set sp.precision (+ (* sp.precision 10) (cast i64 (- s.data[i] #\0))))
          (+= i 1)))
      (when (< i s.len) (set sp.kind s.data[i]))
      (return sp)))
  (defn has [s str] [c u8] -> bool
    (for [x :in s] (when (= x c) (return true)))
    (return false))
  (defn digit [c u8] -> bool (return (and (>= c #\0) (<= c #\9))))

  ; a value, then padding to the spec's width
  (defn show-padded [b (ptr buffer)] [a any] [sp (ptr spec)]
    (let [tmp {buffer}] [t a.type]
         [numeric (or (= t.kind type-kind.int) (= t.kind type-kind.float))]
      (show (addr tmp) a.data t sp false)
      (let [n tmp.len] [pad (if (> sp.width n) (- sp.width n) 0)]
           [align (if (!= sp.align 0) sp.align (if numeric #\> #\<))]
        (cond ((and sp.zero numeric (= sp.align 0))      ; 0 padding goes after the sign
               (let [neg (and (> n 0) (or (= tmp.data[0] #\-) (= tmp.data[0] #\+)))]
                 (when neg (push-byte b tmp.data[0]))
                 (for [_i u64 0 pad] (push-byte b #\0))
                 (if neg (push-bytes b (+ tmp.data 1) (- n 1)) (push-bytes b tmp.data n))))
              ((= align #\<)
               (push-bytes b tmp.data n)
               (for [_i u64 0 pad] (push-byte b sp.fill)))
              ((= align #\^)
               (for [_i u64 0 (/ pad 2)] (push-byte b sp.fill))
               (push-bytes b tmp.data n)
               (for [_i u64 0 (- pad (/ pad 2))] (push-byte b sp.fill)))
              (else
               (for [_i u64 0 pad] (push-byte b sp.fill))
               (push-bytes b tmp.data n))))
      (release (addr tmp))))

  ; the integer at p (of size bytes), widened to 128 bits
  (defn read-int [p (ptr void)] [size u64] [signed bool] -> i128
    (cond ((= size 1) (return (if signed (cast i128 (deref (cast (ptr i8) p))) (cast i128 (deref (cast (ptr u8) p))))))
          ((= size 2) (return (if signed (cast i128 (deref (cast (ptr i16) p))) (cast i128 (deref (cast (ptr u16) p))))))
          ((= size 4) (return (if signed (cast i128 (deref (cast (ptr i32) p))) (cast i128 (deref (cast (ptr u32) p))))))
          ((= size 8) (return (if signed (cast i128 (deref (cast (ptr i64) p))) (cast i128 (deref (cast (ptr u64) p))))))
          (else (return (deref (cast (ptr i128) p))))))

  ; x in base 2..16 (u128 digits, a - for a negative signed x)
  (defn show-int [b (ptr buffer)] [x i128] [signed bool] [size u64] [base u32] [upper bool]
    (let [digits (array u8 130)] [n u64 0] [neg (and signed (< x 0))]
         [bits (bitcast u128 x)]                    ; (two's complement)
         [u (if neg (- (cast u128 0) bits) bits)]
         [chars (if upper "0123456789ABCDEF" "0123456789abcdef")]
      (when (and (not signed) (< size 16))   ; (an unsigned value smaller than 128 bits: only its own bits)
        (set u (& u (- (<< (cast u128 1) (* size 8)) 1))))
      (when (and (!= base 10) neg)               ; other bases show the bits
        (set neg false)
        (set u (if (< size 16) (& bits (- (<< (cast u128 1) (* size 8)) 1)) bits)))
      (while true
        (set digits[n] chars.data[(cast u64 (% u (cast u128 base)))])
        (+= n 1)
        (set u (/ u (cast u128 base)))
        (when (= u 0) (break)))
      (when neg (push-byte b #\-))
      (while (> n 0)
        (-= n 1)
        (push-byte b digits[n]))))

  (defn show-float [b (ptr buffer)] [x f64] [sp (ptr spec)]
    (let [tmp (array i8 64)]
         [n (cond ((= sp.kind #\e) (libc::snprintf (addr tmp[0]) 64 c"%.*e" (if (< sp.precision 0) 6 (cast i32 sp.precision)) x))
                  ((>= sp.precision 0) (libc::snprintf (addr tmp[0]) 64 c"%.*f" (cast i32 sp.precision) x))
                  (else (libc::snprintf (addr tmp[0]) 64 c"%g" x)))]
      (push-bytes b (cast (ptr u8) (addr tmp[0])) (cast u64 n))))

  (defn show-text [b (ptr buffer)] [s str] [sp (ptr spec)] [quoted bool]
    (let [n (if (and (>= sp.precision 0) (< (cast u64 sp.precision) s.len)) (cast u64 sp.precision) s.len)]
      (when quoted (push-byte b #\"))
      (push-bytes b s.data n)
      (when quoted (push-byte b #\"))))

  (defn show-seq [b (ptr buffer)] [p (ptr u8)] [n u64] [et (ptr type-info)]
    (let [none {spec :fill #\space :precision -1}]
      (push-byte b #\[)
      (for [i u64 0 n]
        (when (> i 0) (push-byte b #\space))
        (show b (cast (ptr void) (+ p (* i et.size))) et (addr none) true))
      (push-byte b #\])))

  ; the value at p, of type t; nested: inside a struct or sequence (strs quoted)
  (defn show [b (ptr buffer)] [p (ptr void)] [t (ptr type-info)] [sp (ptr spec)] [nested bool]
    (let [k t.kind] [bytes (cast (ptr u8) p)] [none {spec :fill #\space :precision -1}]
      (cond ((= k type-kind.bool) (push-str b (if (deref (cast (ptr bool) p)) "true" "false")))
            ((= k type-kind.int)
             (let [x (read-int p t.size t.signed)]
               (case sp.kind
                 (#\x (show-int b x t.signed t.size 16 false))
                 (#\X (show-int b x t.signed t.size 16 true))
                 (#\b (show-int b x t.signed t.size 2 false))
                 (#\o (show-int b x t.signed t.size 8 false))
                 (else (show-int b x t.signed t.size 10 false)))))
            ((= k type-kind.float)
             (show-float b (if (= t.size 4) (cast f64 (deref (cast (ptr f32) p))) (if (= t.size 8) (deref (cast (ptr f64) p)) (cast f64 (deref (cast (ptr f80) p)))))
                         sp))
            ((= k type-kind.str) (show-text b (deref (cast (ptr str) p)) sp (or nested (= sp.kind #\?))))
            ((= k type-kind.ptr)
             (let [q (deref (cast (ptr (ptr u8)) p))]
               (if (and (= t.elem.kind type-kind.int) (= t.elem.size 1) t.elem.signed (!= q null))   ; a C string
                   (show-text b {str :data q :len (libc::strlen (cast (ptr i8) q))} sp (or nested (= sp.kind #\?)))
                   (let [tmp (array i8 32)] [n (libc::snprintf (addr tmp[0]) 32 c"%p" q)]
                     (push-bytes b (cast (ptr u8) (addr tmp[0])) (cast u64 n))))))
            ((or (= k type-kind.array) (= k type-kind.simd)) (show-seq b bytes t.len t.elem))
            ((= k type-kind.slice)
             (let [s (deref (cast (ptr (slice u8)) p))]
               (show-seq b s.data s.len t.elem)))
            ((= k type-kind.enum)
             (let [x (cast i64 (read-int p t.size t.signed))] [found false]
               (for [i u64 0 t.values.len]
                 (when (and (not found) (= t.values[i] x))
                   (push-str b t.name) (push-byte b #\.) (push-str b t.names[i])
                   (set found true)))
               (unless found (show-int b (cast i128 x) true 8 10 false))))
            ((= k type-kind.variant)
             (let [tag (deref (cast (ptr u32) (+ bytes t.fields[0].offset)))]
               (if (>= (cast u64 tag) t.names.len)
                   (push-str b "<bad variant>")
                   (let [c t.cases[tag]]
                     (when (!= c null) (push-byte b #\())
                     (push-str b t.name) (push-byte b #\.) (push-str b t.names[tag])
                     (when (!= c null)
                       (let [payload (+ bytes t.fields[1].offset)]
                         (for [f :in c.fields]
                           (push-byte b #\space)
                           (show b (cast (ptr void) (+ payload f.offset)) f.type (addr none) true)))
                       (push-byte b #\)))))))
            ((= k type-kind.struct)
             (push-byte b #\{)
             (push-str b t.name)
             (for [f :in t.fields]
               (push-str b " :") (push-str b f.name) (push-byte b #\space)
               (show b (cast (ptr void) (+ bytes f.offset)) f.type (addr none) true))
             (push-byte b #\}))
            ((= k type-kind.union) (push-str b "<union ") (push-str b t.name) (push-byte b #\>))
            ((= k type-kind.closure) (push-str b "<closure>"))
            (else (push-str b "<") (push-str b t.name) (push-byte b #\>))))))
