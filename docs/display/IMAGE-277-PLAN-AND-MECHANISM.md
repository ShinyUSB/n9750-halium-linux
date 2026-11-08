# Image 277 — the missing secure-mode exit (`CP_SET_SECURE_MODE`)

Status: kernel patched and building. Nothing flashed yet.

## The gap, stated exactly

Our tree leaves the GPU in **secure mode** and never takes it out the way the
hardware allows. Live state has been constant for 260+ images:

    SECVID134: TRUST_CNTL=0x1 TSB_CNTL=0x0 TSB_BASE=0x0 SIZE=0x0   (status 0x1)

`TRUST_CNTL=0x1` is "secure". The TSB (the secure translation window) is
empty. `experimental_nonlegacy=1` means the tree **skips** the host write, so
the GPU has been left secure for the whole project.

## What every working driver does instead

Stock CAF KGSL — `drivers/gpu/msm/adreno.c:1870-1875`:

```c
/* GPU comes up in secured mode, make it unsecured by default */
if (adreno_dev->zap_handle_ptr)
    ret = adreno_switch_to_unsecure_mode(adreno_dev, rb);
else
    adreno_writereg(adreno_dev, ADRENO_REG_RBBM_SECVID_TRUST_CONTROL, 0x0);
```

and `adreno_switch_to_unsecure_mode()` (`adreno.c:1833-1853`) puts
`cp_secure_mode(dev, cmds, 0)` in the **ringbuffer** — i.e. it asks the GPU's
own CP to write the register — then `adreno_ringbuffer_submit_spin()`.

Mainline a5xx — `drivers/gpu/drm/msm/adreno/a5xx_gpu.c` (a5xx_hw_init), with
the comment that names our exact symptom:

```c
/*
 * Try to load a zap shader into the secure world. If successful
 * we can use the CP to switch out of secure mode. If not then we
 * have no resource but to try to switch ourselves out manually. If we
 * guessed wrong then access to the RBBM_SECVID_TRUST_CNTL register will
 * be blocked and a permissions violation will soon follow.
 */
ret = a5xx_zap_shader_init(gpu);
if (!ret) {
        OUT_PKT7(gpu->rb[0], CP_SET_SECURE_MODE, 1);
        OUT_RING(gpu->rb[0], 0x00000000);
        gpu->funcs->flush(gpu);
        if (!a5xx_idle(gpu))
                return -EINVAL;
} else {
        dev_warn_once(..., "Zap shader not enabled - using SECVID_TRUST_CNTL instead\n");
        gpu_write(gpu, REG_A5XX_RBBM_SECVID_TRUST_CNTL, 0x0);
}
```

Mainline a6xx does the same thing (verified against upstream master by
independent research): the zap-init branch emits `CP_SET_SECURE_MODE = 0`; the
**host** write happens only on the `-ENODEV` fallback.

**Our 4.14 msm tree has neither.** `grep CP_SET_SECURE_MODE
drivers/gpu/drm/msm/adreno/*.c` → nothing. So the tree does the "no zap"
fallback, which on this silicon is the guess that goes wrong: image 273 wrote
the register from the host and the machine wedged mid-hw_init ("before"
printed, "after" never did).

### Why this also explains the asymmetry that killed earlier hypotheses

The CP's own memory path is **not** the path that is gated:

* the fence works — `CP_EVENT_WRITE` lands in memptrs (`memptrs->fence 0→1→2`),
  so the CP *can* write DDR;
* the RB/SP/2D paths never have (0/64 SSBO, 0/6 FBO, 0/4096 CP_BLIT), because
  the shader/render data path is what a secure-mode GPU routes through the
  (here empty) TSB.

"A secure gate would have stopped the CP too" was the reason the gate was
dismissed (image 243 / ANALYSIS-FOUR-CANDIDATES §D). That reasoning does not
hold: the CP has its own aperture (programmed separately, and stock KGSL even
makes an SCM call for it), while the 3D data path is exactly what SECVID/TSB
governs.

### Corroboration from the working Android boot

The same bootloader/TZ/secure state, a stock-derived kernel, and KGSL: the GPU
renders. KGSL's start path is the one above, and it *always* has a zap handle,
so it always takes the CP packet branch — never the host write.

### Corroboration from a sibling device

`gts6l-mainline-linux` (Galaxy Tab S6, SM8150, a640): with linux-firmware's
Qualcomm **test-signed** `a640_zap`, Samsung TZ rejects PAS-auth and
*"the GPU never leaves secure mode"*; supplying the Samsung-signed
`a640_zap.mdt` fixes it. Same part, same failure mode, same resolution
mechanism.

## The change (image 277)

`a6xx_gpu.c`, in `a6xx_hw_init()` immediately after `a6xx_cp_init()` — which
has already proven CPU→ring→CP→idle — gated on a new parameter
`a6xx.cp_secure_off=1`:

```c
if (cp_secure_off) {
        struct msm_ringbuffer *ring = gpu->rb[0];

        pr_info("SECVID277: CP path: before TRUST_CNTL=0x%x TSB_CNTL=0x%x TSB_BASE=0x%x SIZE=0x%x\n", ...);
        OUT_PKT7(ring, CP_SET_SECURE_MODE, 1);
        OUT_RING(ring, 0x00000000);
        a6xx_flush(gpu, ring);
        if (!a6xx_idle(gpu, ring)) { /* loud timeout, RPTR/WPTR/STATUS/INT0 */ }
        pr_info("SECVID277: CP path: after TRUST_CNTL=0x%x TSB_CNTL=0x%x TSB_BASE=0x%x SIZE=0x%x\n", ...);
} else if (secure_probe == 1 || experimental_nonlegacy) { /* unchanged */ }
```

One variable: the CPU host write is **not** used in this image. The before/
after readback makes the test falsifiable even if the render does not change.

`CP_SET_SECURE_MODE` is `0x66`; `OUT_PKT7` already exists in `adreno_gpu.h`
and is used by the tree's own `a6xx_cp_init()`, so the packet is emitted
exactly as this driver emits every other kernel-side command.

## The run (run277.sh)

1. load the ZAP **first** (`soc:qcom,kgsl-hyp/fw_load`) — the CP is only
   authorised to touch the register after secure firmware authorises it;
2. upstream bring-up chain → `SECVID277: before/after` in dmesg;
3. `secvid_cp.py` — the **never-tested** discriminator: a raw DRM submit of
   `CP_MEM_WRITE` alone, then `CP_SET_SECURE_MODE(0)+CP_MEM_WRITE`, each read
   back through the same CPU mapping, each proven submitted (ring wptr) and
   retired (fence fd);
4. `wrt2.py` — the shader/SSBO/RB oracle.

## Falsification

* **after TRUST_CNTL == 0x0 and render works** → defect was the missing CP
  secure-mode exit; fix as stock does (no TZ/bootloader cooperation needed).
* **after TRUST_CNTL still 0x1** → the CP write was not authorised: the ZAP is
  not actually authenticated by Samsung TZ on this device, and the next step is
  the Samsung-signed `a640_zap` (gts6l's resolution), not more driver work.
* **a6xx_idle timeout / wedge** → the packet is not accepted from this ring
  state; report the RPTR/WPTR/STATUS/INT0 dump rather than guessing.
