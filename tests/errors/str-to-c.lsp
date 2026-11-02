; error: a str is not a C string: write this literal as c"..."
(import "libc")
(defn main () i32 (libc::puts "hello") 0)
