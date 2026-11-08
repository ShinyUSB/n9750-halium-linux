# Image 167 — Mesa UAPI compat (unblock userspace)

## Why (empirical, from the device itself)
`eglinfo` + strace on-device showed Mesa never gets past driver load:
our DRM driver registers as `"msm_drm"` while Mesa looks up
`__driDriverGetExtensions_msm` (from `msm_dri.so`) — LD_DEBUG proves
the name is the sole blocker (`undefined symbol:
__driDriverGetExtensions_msm_drm`). Upstream registers `"msm"`.
Diffing our 4.14 UAPI against upstream `msm_drm.h` then gave the full
gap list: no submitqueues, no UBWC/layout params, 8-byte-shorter
submit struct (Mesa 22.3 writes `queueid`), 16-byte GEM_INFO (Mesa
sends 24). drm_ioctl tolerates bigger user structs, so appended
fields + new ioctls are strictly additive.

## Diff
- `msm_drv.c`: drm_driver `.name = "msm"` (platform driver untouched).
- UAPI: new params PRIORITIES/FAULTS/SUSPENDS/HIGHEST_BANK_BIT/
  UBWC_SWIZZLE/MACROTILE_MODE; `queueid` appended to submit;
  SUBMITQUEUE_NEW/CLOSE (0x0A/0x0B) + struct; SET_NAME info bit.
- `adreno_gpu.c`: new param cases (PRIORITIES=1, zero counters,
  HBB=15 from DT, swizzle/macrotile=0 with noubwc fallback noted).
- `msm_drv.c`: submitqueue handlers (id=1, single legacy ring;
  CLOSE no-op) with RENDER_ALLOW; SET_NAME accepted (debug-only).
- Untouched: submit fast path (queueid ignored), all 160–166
  behavior. eglinfo needs only priv->gpu (set even on suppress),
  so screen-probe works without survival.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- `msm_drv.o` carries both submitqueue handlers.
- Packaged with 160 cmdline.
- Image 167 `167-halium-boot-a6xx-mesauapi.img`
  sha256 `7613c464efc45e7c0c3da18f746adad2dce9a0f1abec6f316c55c5832816fa62`.
  (Rev 2: PRIORITIES 1→3 — Mesa validates pipe prio against it and
  refused prio 1 with a single level; single queue still backs all.)

## Readout
- `eglinfo` shows freedreno/a640 screen → UAPI sufficient; next is
  surfaceless render on a surviving run (CP IRQ verify first).
- Next ioctl failure names itself via strace (iterate cheaply).

No flash performed (166 first: survival/recovery decides the
testbed); stopping.
Also staged on-device: `msm_drm_dri.so -> msm_dri.so` symlink
(remove after the rename lands; harmless otherwise).
