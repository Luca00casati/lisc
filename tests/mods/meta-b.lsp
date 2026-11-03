; tests/metas.lsp: the same helper name, another meaning
(private
  (defmeta scale (x) (* x 100)))
(defmacro hundreds! (x) (scale x))
