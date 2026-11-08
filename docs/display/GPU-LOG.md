# d2q GPU investigation log (2026-09-29) - VERDICT: no native HW path, Pixman stays

## Hardware (M12)
- Adreno 640 v2 (`/sys/class/kgsl/kgsl-3d0/gpu_model`), Snapdragon 855+.
- Kernel GPU driver: KGSL ONLY (`CONFIG_QCOM_KGSL=y`, `/dev/kgsl-3d0` live,
  0% idle). No MSM adreno: no `CONFIG_DRM_MSM_GPU`, no DT adreno node,
  no adreno probe in dmesg. Tree has adreno/a2xx-a5xx only (NO a6xx).
- renderD128 belongs to msm_drm WITHOUT msm_gpu subunit (display-only).
- DT includes sm8150-gpu.dtsi for KGSL (`a640_zap` firmware ref).

## Render node (M13)
- `/dev/dri/renderD128` (root:render) opens fine; GETPARAM GPU_ID ->
  -ENXIO by code inspection (`priv->gpu==NULL`, msm_drv.c:1130+).
  This is the fatal answer for Freedreno-on-MSM.

## Mesa (M14/M15)
- Mesa 22.3.6 (bookworm): EGL/GBM/GLES/DRI present, incl. msm_dri.so
  (freedreno) and kms_swrast. No llvmpipe confusion: eglinfo GBM platform
  = `kms_swrast` (software) - NOT success, recorded as baseline.
- Loader quirk: vendor kernel advertises DRM name `msm_drm` (upstream:
  `msm`), so Mesa looks for nonexistent `msm_drm_dri.so`. A temporary
  symlink was created for diagnosis and REVERTED (verified absent).
- Even past the name: needs `__driDriverGetExtensions_msm_drm` (absent)
  and then GPU_ID (ENXIO). Two independent hard blocks.
- `MESA_LOADER_DRIVER_OVERRIDE={msm,kgsl}`: both end at
  "failed to create dri2 screen". The kgsl entry point exists in the .so
  but no EGL loader path reaches a KGSL fd on this system.

## Weston GL experiment (diagnostic, reverted)
- Stopped pixman service, ran Weston WITHOUT --use-pixman:
  gl-renderer.so loads, `GL renderer: llvmpipe (LLVM 15.0.6)`,
  OpenGL ES 3.2, desktop VISIBLE, DSI-1 enabled, ~17% CPU for weston.
- Proves EGL->GBM->WestonGL->DRM plumbing works; only HW driver missing.
- Restored: pixman service active, symlink removed, flower+touch verified.

## libhybris verdict (NOT pursued - justified)
- Weston gl-renderer requires EGL+GBM. Vendor Adreno EGL has no GBM
  platform (gralloc-based); bridging it needs a gralloc-GBM translation
  that does not exist in-tree. So libhybris cannot serve Weston either.
- The only real HW path is an a6xx MSM backport (no sources in tree,
  GMU/ZAP/preemption work, high brick risk for display). Out of scope.

## Changes this session
- Installed: mesa-utils (8.5.0-1) [was already: weston, wayland-utils,
  libdrm-tests, libinput-tools, evtest, kbd].
- No kernel, service, config, or firmware changes. No Android touched.
- Pixman fallback: untouched throughout, verified after every experiment.

## Status: WORKING=Pixman+touch+autostart+console+net; NOT TESTED=wake-on-touch-blank;
         FUTURE=a6xx backport (huge), brightness, DPI/orientation polish.
