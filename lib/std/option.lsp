; std/option — values that may be missing, results that may be errors:
;
;   (option T): (option.some x) or option.none
;   (result T E): (result.ok x) or (result.err e)
;
; take them apart with match:
;   (match (find xs 7) ((some i) ...) (none ...))
; or with these:
;   (or-else o d)    o's value, or d          (is-some o) (is-ok r)   bool

(variant (option T) (some [value T]) none)
(variant (result T E) (ok [value T]) (err [error E]))

(defn (or-else T) [o (option T)] [d T] T
  (match o ((some x) x) (none d)))

(defn (is-some T) [o (option T)] bool
  (match o (some true) (none false)))

(defn (is-ok T E) [r (result T E)] bool
  (match r (ok true) (err false)))
