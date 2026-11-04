; hygienic macros: a template's names never clash with the use site's
(import "libc")
(using libc)

(const scale 10)

(defmacro swap! (a b)                     ; no gensym needed
  `(let [tmp ,a] (set ,a ,b) (set ,b tmp)))

(defmacro scaled! (x)                      ; `scale` here is the global constant
  `(* ,x scale))

(defmacro sum-twice! (x)                   ; binds `acc`, uses another macro inside
  `(let [acc 0]
     (set acc (+ acc ,x))
     (set acc (+ acc ,x))
     acc))

(defmacro aif! (c then else)               ; captures `it` on purpose: ,'it is unmarked
  `(let [,'it ,c] (if (!= ,'it 0) ,then ,else)))

(defmacro with-counter! (name &body body)  ; the caller names the variable
  `(let [,name 0] ,@body ,name))

(defmacro rotate! (a b c)                 ; a macro whose expansion uses swap! twice
  `(do (swap! ,a ,b) (swap! ,b ,c)))

(defn main [] -> i32
  ; the user's own `tmp` is swapped, not confused with the macro's
  (let [tmp 1] [other 2]
    (swap! tmp other)
    (printf c"swap %d %d\n" tmp other))
  ; a local named `scale` does not change what the macro means
  (let [scale 3]
    (printf c"scaled %d (local scale %d)\n" (scaled! 2) scale))
  ; the argument mentions the user's `acc`: it is not the macro's `acc`
  (let [acc 5]
    (printf c"sum-twice %d\n" (sum-twice! acc)))
  (printf c"aif %d %d\n" (aif! (* 6 7) (+ it 1) -1) (aif! 0 (+ it 1) -1))
  (printf c"counter %d\n" (with-counter! n (set n (+ n 4)) (set n (* n 10))))
  (let [x 1] [y 2] [z 3]
    (rotate! x y z)
    (printf c"rotate %d %d %d\n" x y z))
  (return 0))
