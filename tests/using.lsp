; C modules (libc, math, import-c) and using: file-wide or for one block
(import "libc")
(import "math")
(import-c "drm/i915_drm.h" :as i915)

(defn hyp [a f32] [b f32] f32
  (using math)                            ; only in this function
  (sqrtf (+ (* a a) (* b b))))

(defn main [] i32
  (libc::printf c"qualified: %g %ld\n" (math::cos 0.0) (libc::strlen c"four"))
  (let [gp {i915::drm_i915_getparam :param i915::I915_PARAM_CHIPSET_ID}]
    (using libc)                          ; in this let block
    (printf c"block: %g %d %lx\n" (cast f64 (hyp 3.0 4.0)) gp.param i915::DRM_IOCTL_I915_GETPARAM))
  0)
