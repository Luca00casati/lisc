; error: atomic-add needs an integer or pointer place, got f32
(def x f32 0.0)
(defn main () i32 (atomic-add x 1.0) 0)
