; error: unknown name 'hidden'
(defmacro make-hidden (&body body) `(let ((hidden 7)) ,@body))
(defn main () i32 (make-hidden hidden))
