; Ask the i915 kernel driver which GPU this is: open a render node and
; issue DRM_IOCTL_I915_GETPARAM, all with types and numbers from the headers.
(c-include "stdio.h" "fcntl.h" "unistd.h" "sys/ioctl.h" "drm/i915_drm.h")

(defn main () i32
  (let ((fd (open "/dev/dri/renderD128" O_RDWR)))
    (when (< fd 0)
      (perror "/dev/dri/renderD128")
      (return 1))
    (let ((id i32 0)
          (gp (make drm_i915_getparam :param I915_PARAM_CHIPSET_ID :value (addr id))))
      (if (= (ioctl fd DRM_IOCTL_I915_GETPARAM (addr gp)) 0)
          (printf "i915 chip id 0x%04x\n" id)
          (perror "I915_GETPARAM")))
    (close fd))
  0)
