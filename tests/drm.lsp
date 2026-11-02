; kernel uapi headers: struct layouts and ioctl numbers straight from
; <drm/drm_mode.h> and <drm/i915_drm.h> (values checked against a C program)
(import "libc")
(using libc)
(import-c "drm/drm.h" "drm/drm_mode.h" "drm/i915_drm.h" :as uapi)
(using uapi)

(defn main () i32
  (printf c"%lx %lx %lx\n" DRM_IOCTL_MODE_GETRESOURCES DRM_IOCTL_MODE_SETCRTC DRM_IOCTL_MODE_GETCONNECTOR)
  (printf c"%lx %lx %lx\n" DRM_IOCTL_I915_GETPARAM DRM_IOCTL_I915_GEM_EXECBUFFER2 DRM_IOCTL_GEM_CLOSE)
  (printf c"%lu %lu %lu %lu\n" (sizeof drm_mode_modeinfo) (sizeof drm_mode_crtc)
          (sizeof drm_i915_gem_exec_object2) (sizeof drm_i915_gem_execbuffer2))
  (printf c"%d %d %d\n" I915_PARAM_CHIPSET_ID I915_GEM_DOMAIN_CPU DRM_MODE_PAGE_FLIP_EVENT)
  0)
