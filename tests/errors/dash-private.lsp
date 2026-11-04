; error: defn- is gone: private definitions go in (private (defn ...) ...)
(defn- helper [] i32 1)
(defn main [] -> i32 (return (helper)))
