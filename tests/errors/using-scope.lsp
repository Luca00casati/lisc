; error: unknown function 'puts'
(import "libc")
(defn main () i32
  (let ((x 1)) (using libc) (puts c"inside"))
  (puts c"outside: libc is not in use here")
  0)
