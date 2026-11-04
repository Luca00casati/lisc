; error: cannot pass a str to a variadic function
(import "libc")
(defn main [] -> i32 (let [s "hi"] (libc::printf c"%s\n" s)) (return 0))
