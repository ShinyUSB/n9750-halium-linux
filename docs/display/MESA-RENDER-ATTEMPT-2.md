# First pixels attempt #2 — a real GLES 3.1 context on FD640

State: kernel `#126`, image 184, phone alive, GPU loaded (`ALL 10 PASS`).

## Headline

Mesa created a **real OpenGL ES 3.1 context on the Adreno 640** and accepted a
clear into it:

```
GL_VENDOR    = freedreno
GL_RENDERER  = FD640
GL_VERSION   = OpenGL ES 3.1 Mesa 22.3.6
GL_GLSL      = OpenGL ES GLSL ES 3.10
framebuffer status=0x8cd5 (COMPLETE)
glClear err=0x0000
glFlush err=0x0000
```

So: EGL init, GBM platform, configless context, renderbuffer storage,
FBO attachment, and command submission all work. **The CPU→GPU path is fully
functional.** What is missing is the last mile — GPU→CPU completion — so no
pixel can be read back yet.

## The actual blocker

Both completion waits block forever:

- `glFinish()` — never returns.
- `glReadPixels()` — never returns.

The reason is visible in `/proc/interrupts`:

```
487:  0  ...  PDC-GIC 332 Level  A640           <-- GPU core IRQ: NEVER FIRES
488:  7  ...  PDC-GIC 336 Level  kgsl_hfi_irq
489:  0  ...  PDC-GIC 337 Level  kgsl_gmu_irq
```

The HFI line counted 7 interrupts — exactly the bring-up handshake — which
proves interrupt *routing* is healthy and the GIC/PDC path works for this GPU.
But the **core IRQ is at zero**, so the driver is never told the command
processor finished.

The chain that is dead:

```
glFinish/glReadPixels
  -> waits on fence
  -> msm_gpu_retire()            <- only caller of the fence update
  -> a6xx_irq()                  <- a6xx_gpu.c:1258
  -> status & A6XX_..._CP_CACHE_FLUSH_TS
  -> GPU raises core IRQ 487
```

`a6xx_gpu.c:1258-1285` is correct and complete. The device-side unmask is also
present: `a6xx_gpu.c:958` writes `REG_A6XX_RBBM_INT_0_MASK = A6XX_INT_MASK`
inside `a6xx_hw_init`, and `A6XX_INT_MASK` (line 697) includes
`CP_CACHE_FLUSH_TS`. `msm_gpu.c:719` requests the IRQ successfully (no
"failed to request IRQ" anywhere in dmesg), and `msm_gpu.c:266/274` bracket
hw_init with `disable_irq`/`enable_irq`.

So the registers are unmasked, the handler is installed, the line is routed —
and it still never asserts. That leaves two possibilities:

1. **The CP is not executing Mesa's render ring at all.** Our bring-up
   selftest drives the CP with its own scratch ring (`CP_PRE_FLUSH: ring_iova=0x1000
   RB_BASE=0x1000 RB_CNTL=0x20c`) and leaves the ring registers pointing there.
   If the handoff to Mesa's ring does not re-arm the CP, it never fetches.
2. The CP executes but does not signal completion.

**This is not distinguishable from userspace** — there is no `/proc/gpu`, no
`/dev/mem`, and `/sys/kernel/debug/dri/0/gpu` is empty. Answering it needs a
kernel-side read of the CP ring registers. See "Next" below.

Ruled out: SMMU faults (IRQs 490/491/492 all zero — the GPU is not trying and
failing to fetch from a bad address), missing IRQ registration, masked
interrupts, missing device-side unmask.

## Two useful side findings

**1. Image 184's fix works.** The log shows
`a6xx: timestamp: GPU_SET OOB ret=-110, reading anyway` — the OOB handshake
fails, logs, and the read completes anyway. The `D`-state wedge in
`MSM_PARAM_TIMESTAMP` is gone. That fix did its job.

**2. The original `egl-proof.py` segfault was Mesa, not our driver.** The
earlier `RENDER-EXIT=139` was not a driver hang. Mesa 22.3.6 here **segfaults
on the surfaceless and device EGL platforms** right after `eglInitialize`:

- `eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA)` → `eglInitialize`
  returns 1 (1.4), then `eglChooseConfig` returns garbage `num_config`; headless
  weston with `--use-gl` segfaults immediately after logging
  `EGL_KHR_surfaceless_context available`.
- `EGL_PLATFORM_DEVICE_EXT` → same garbage `num_config` from `eglChooseConfig`.
- **`EGL_PLATFORM_GBM_KHR` works perfectly** — `eglChooseConfig` enumerates ~42
  configs, contexts create, GL reports FD640.

GBM is also the platform real Android HAL compositors use, so this is the right
platform regardless. Harness rewritten to use it
(`artifacts/gpu-bringup/harness/egl-gbm-fbo.py`).

## Reproduce

```sh
# over USB RNDIS
python3 /usr/local/bin/egl6.py     # GLES ctx + FBO + glClear + glFinish (hangs at glFinish)
python3 /usr/local/bin/egl7.py     # same but glFlush + fence poll + glReadPixels (hangs at readpixels)
```

Local copies: `artifacts/gpu-bringup/harness/`.
Push helper: `artifacts/gpu-bringup/harness/push-over-rndis.sh` — the RNDIS
link drops its IPv4 whenever the GPU kicks PCIe, so it re-adds
`10.15.19.100/24` and retries with an md5 check.

## Next — image 185: read the CP ring, don't guess

One image, one question: **is the CP advancing Mesa's ring?**

Add an on-demand sysfs probe (same shape as the existing `a6xx_gpu_load`
trigger, so it is safe and off the hot path) that dumps, before and after a
render:

- `REG_A6XX_CP_RB_BASE` / `_HI`, `REG_A6XX_CP_RB_RPTR`, `REG_A6XX_CP_RB_WPTR`,
  `REG_A6XX_CP_RB_CNTL`
- `REG_A6XX_CP_INTERRUPT_STATUS`
- `REG_A6XX_RBBM_INT_0_STATUS`, `REG_A6XX_RBBM_INT_0_MASK`
- `kstat_irqs(gpu->irq)`

Readout:

- **RPTR advances toward WPTR** → the CP is fetching and executing; only the
  completion interrupt is broken. Fix is in the signal path.
- **RPTR stuck at its initial value** → the CP never started on Mesa's ring.
  Fix is the ring handoff: after the bring-up selftest, explicitly re-program
  the real render ring's BASE/RPTR/WPTR and set the `CP_RB_CNTL` enable bit.

Instrumenting before attempting a fix is deliberate — guessing here costs a
flash cycle, and the two fixes are completely different.