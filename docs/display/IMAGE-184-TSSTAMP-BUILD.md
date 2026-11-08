# Image 184 — unblock Mesa's first ioctl (MSM_PARAM_TIMESTAMP)

## Why

First render attempt against a LOADED GPU
(`MESA-RENDER-ATTEMPT-1.md`) won the lottery on ticket 1
(`ALL 10 PASS`, `HWINIT[25] ret=0`, phone responsive the whole time on
the new USB RNDIS channel) — then `egl-proof.py` printed not even
`DEVICES:`. Its python3 was stuck in `D` for minutes:

```
__switch_to → rpm_resume+0x280 → __pm_runtime_resume
            → adreno_get_param+0x110 → msm_ioctl_get_param
            → drm_ioctl_kernel → do_vfs_ioctl → SyS_ioctl
```

`MSM_PARAM_TIMESTAMP` is the FIRST ioctl Mesa issues when it creates a
context. Two things stacked on it:

1. `adreno_get_param()` (adreno_gpu.c:74-84) wrapped the read in
   `pm_runtime_get_sync(&gpu->pdev->dev)` /
   `pm_runtime_put_autosuspend()`. Since 182 the bring-up leaves the GPU
   powered (slumber off), so this power transition is unnecessary — and
   the held ref is what kept the task in `D` inside `rpm_resume`.
2. `a6xx_get_timestamp()` issued `GMU_OOB_GPU_SET` first. The OOB set is
   already bounded (`gmu_poll_timeout`, 10s) and returns non-zero on
   timeout, so it is not an unbounded hang — but on quiet firmware the
   call costs 10s per timestamp and the clear was issued unconditionally
   even when the set never ACKed.

Net: a diagnostics ioctl could hang all of userspace before a surface
existed. Unacceptable for a GPU that still has firmware-flakiness ahead
of it.

## Diff (two files, on top of 183)

- **`a6xx_gpu.c`, `a6xx_get_timestamp()`**: capture the
  `a6xx_gmu_set_oob(GMU_OOB_GPU_SET)` return; on failure log ratelimited
  and read the counter anyway; only `a6xx_gmu_clear_oob()` when we own a
  successful set. Returns 0 always (a stale timestamp beats a wedged
  process).
- **`adreno_gpu.c`, `adreno_get_param()` case `MSM_PARAM_TIMESTAMP`**: drop
  the `pm_runtime_get_sync`/`put_autosuspend` pair. The read no longer
  takes a power transition, so there is nothing to hold a PM ref while an
  OOB handshake runs. Comment marks that a future real suspend path must
  re-add the ref behind a live "is it on" check, not blindly.

Untouched: everything in 181-183 (bark-pet, `enable=0`, no-IPI pet, CPU0
pinning, no panic escalation, softdog/FPUT off, async trigger, quiet
suspend path, TCM-aperture gate, slumber-off default). Cmdline identical
to 183.

## Checks

- build: 0 errors, no adreno/a6xx warnings.
- `.config`: softdog/FPUT/panic-on-oops/bite-on-panic all still unset.
- `Image-dtb` contains the new `a6xx: timestamp: GPU_SET OOB ret=%d,
  reading anyway` string; keeps `BARK181` and the async-trigger string.
- packaged with the same ramdisk/board/header/cmdline as 180-183.

Image 184 `184-halium-boot-a6xx-tsstamp.img`
sha256 `45f2029ba127014e4560ad412ae3f0e0bf4160b4708b39be5182354db6fb4e8d`.

## Readout

- `RENDERER:` / `VERSION:` / `PIXEL: [255, 0, 0, 255]` / `RENDER-OK` in
  `/var/log/egl184.log` → **first pixels on the Adreno 640.** Then the
  real work: sustained rendering, thermal/power policy, devfreq.
- Still hangs, but now with `GPU_SET OOB ret=-110` in the log first →
  timestamp fixed; the wedge moved later (likely the CP IRQ path, never
  verified live).
- Still hangs with no OOB error → the hang is the MMIO read itself
  (same class as the TCM aperture, but on the always-on AHB). Then the
  read needs the same "is the GPU really up" guard.

No flash performed (needs Download mode); stopping.