; tests/metas.lsp: a helper named like meta-b's, used by this module's macro
(defmeta- scale (x) (* x 10))
(defmeta shout (s) (str s "!"))
(defmacro tens (x) (scale x))
