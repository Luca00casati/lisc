; std/map — hash maps, (map K V):
;
;   (put m k v)      add or replace          (get m k)     (ptr V), or null
;   (has m k)        bool                    (remove m k)  bool: was it there
;   (release m)      free it (empty again)   (each m f)    f called with each k and v
;
; m is a (ptr (map K V)); a zero map is an empty one. Keys: strs by their
; contents, anything else by its bytes. Open addressing with linear probing; removal shifts entries back, so
; there are no tombstones.
(import "libc")

(struct (map K V) [keys (ptr K)] [vals (ptr V)] [used (ptr bool)] [len u64] [cap u64])

(private
  (defn hash-bytes [p (ptr u8)] [n u64] -> u64
    (let [h u64 0xcbf29ce484222325]
      (for [i u64 0 n]
        (set h (* (^ h p[i]) 0x100000001b3)))
      (return (mix h))))
  (defn mix [x u64] -> u64                         ; (splitmix64's finalizer)
    (set x (* (^ x (>> x 30)) 0xbf58476d1ce4e5b9))
    (set x (* (^ x (>> x 27)) 0x94d049bb133111eb))
    (return (^ x (>> x 31))))

  ; a key's hash and equality, chosen by its type: a str by its contents,
  ; anything else by its bytes
  (defop key-hash [x str] -> u64 (return (hash-bytes x.data x.len)))
  (defop (key-hash T) [x T] -> u64 (return (hash-bytes (cast (ptr u8) (addr x)) (sizeof T))))
  (defop same-key [a str] [b str] -> bool
    (return (and (= a.len b.len) (= (libc::memcmp a.data b.data a.len) 0))))
  (defop (same-key T) [a T] [b T] -> bool (return (= (libc::memcmp (addr a) (addr b) (sizeof T)) 0)))

  ; where k is, or the free slot where it would go (cap > 0)
  (defn (slot K V) [m (ptr (map K V))] [k K] -> u64
    (let [mask (- m.cap 1)] [i (& (key-hash k) mask)]
      (while (and m.used[i] (not (same-key m.keys[i] k)))
        (set i (& (+ i 1) mask)))
      (return i)))

  (defn (grow K V) [m (ptr (map K V))]
    (let [old (deref m)] [cap (if (= old.cap 0) 16 (* old.cap 2))]
      (set m.cap cap)
      (set m.len 0)
      (set m.keys (cast (ptr K) (libc::calloc cap (sizeof K))))
      (set m.vals (cast (ptr V) (libc::calloc cap (sizeof V))))
      (set m.used (cast (ptr bool) (libc::calloc cap 1)))
      (when (or (= m.keys null) (= m.vals null) (= m.used null)) (libc::abort))
      (for [i u64 0 old.cap]
        (when old.used[i] (put m old.keys[i] old.vals[i])))
      (libc::free old.keys)
      (libc::free old.vals)
      (libc::free old.used))))

(defn (put K V) [m (ptr (map K V))] [k K] [v V]
  (when (>= (* (+ m.len 1) 4) (* m.cap 3)) (grow m))   ; (at most 3/4 full)
  (let [i (slot m k)]
    (unless m.used[i]
      (set m.used[i] true)
      (set m.keys[i] k)
      (+= m.len 1))
    (set m.vals[i] v)))

(defn (get K V) [m (ptr (map K V))] [k K] -> (ptr V)
  (when (= m.cap 0) (return null))
  (let [i (slot m k)]
    (return (if m.used[i] (addr m.vals[i]) null))))

(defn (has K V) [m (ptr (map K V))] [k K] -> bool (return (!= (get m k) null)))

(defn (remove K V) [m (ptr (map K V))] [k K] -> bool
  (when (= m.cap 0) (return false))
  (let [mask (- m.cap 1)] [i (slot m k)]
    (unless m.used[i] (return false))
    (set m.used[i] false)
    (-= m.len 1)
    ; later entries of the run move back into the hole when their home slot
    ; isn't between the hole and them, so lookups still reach them
    (let [j (& (+ i 1) mask)]
      (while m.used[j]
        (let [home (& (key-hash m.keys[j]) mask)]
          (when (if (<= i j) (or (<= home i) (> home j)) (and (<= home i) (> home j)))
            (set m.keys[i] m.keys[j])
            (set m.vals[i] m.vals[j])
            (set m.used[i] true)
            (set m.used[j] false)
            (set i j)))
        (set j (& (+ j 1) mask))))
    (return true)))

(defn (release K V) [m (ptr (map K V))]
  (libc::free m.keys)
  (libc::free m.vals)
  (libc::free m.used)
  (set (deref m) null))

; f called with every entry, in no particular order: (each m (lambda [k K] [v V] ...))
(defn (each K V F) [m (ptr (map K V))] [f F]
  (for [i u64 0 m.cap]
    (when m.used[i] (f m.keys[i] m.vals[i]))))
