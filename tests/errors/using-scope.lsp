; error: unknown function 'puts'
(import "libc")
(defn main () i32
  (let ((x 1)) (using libc) (puts "inside"))
  (puts "outside: libc is not in use here")
  0)
