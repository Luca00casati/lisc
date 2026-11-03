; error: no definition of area takes (area i32)
(defop area [r f32] f32 (* r r))
(defn main () i32 (discard (area (cast i32 2))) 0)
