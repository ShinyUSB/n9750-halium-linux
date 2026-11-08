# Image 204 — the recover-path `pm_runtime` wedge (the silent death is a deadlock)

Date: 2026-11-11. Built, packaged, **not yet flashed**. One-line change to
`recover_worker()`.

## Two corrections this session made to the record

### 1. Image 203 WORKED. The oops is gone.

Image 203 (`a6xx_last_fence`) fixed the NULL function pointer. Since the flash:
**zero** `Unable to handle` / `Internal error` / `jopp_springboard` in dmesg,
and the phone has stayed up across many GL runs. `a6xx_last_fence` is present
in `/proc/kallsyms`. The hangcheck timer, which used to oop, now runs cleanly
and prints `HANGCHK141` every second.

### 2. "Zero GEM_SUBMIT ever" was a PHANTOM — submits were always happening.

`dmesg | grep -c GEM_SUBMIT` returned 0, and I recorded that as "no submit has
ever reached the kernel". **This kernel contains no `GEM_SUBMIT` printk at
all.** `grep -rn GEM_SUBMIT drivers/gpu/drm/msm/*.c` returns only comments.
The metric was meaningless.

The real evidence is the hangcheck line itself:

```
HANGCHK141: enter fence=0 stored=0 submitted=1
HANGCHK141: LOCKUP fence=0 submitted=1 -> recover queued
```

`submitted=1` is `gpu->fctx->last_fence`. That counter is incremented in
exactly one place — `msm_fence_alloc()` (`msm_fence.c:154`), called from
`prepare_submit()` (`msm_gem_submit.c:537`). **So `submitted=1` means a
`MSM_GEM_SUBMIT` ioctl completed and `msm_gpu_submit()` ran, reaching
`a6xx_submit()` and writing to the ring.** Mesa was submitting all along.

This retires `ROOT-CAUSE-MESA-NEVER-SUBMITS.md` and
`CLEAR-DRAW-BOTH-SWALLOWED.md`. The Mesa "silently swallows everything" theory
was built on a grep for a nonexistent string. The absence of freedreno `DBG`
lines is real but has a mundane explanation worth revisiting later (those DBGs
are gated on `FD_DBG(MSGS)` and the binary is a release build with the strings
present but the specific call sites evidently not reached — a separate, much
smaller question, and NOT what was blocking submits).

## What image 203 actually revealed

With the oops fixed, the next layer became visible — and it is the same
deadlock image 189 fixed in `msm_gpu_submit()`, still unfixed in the recover
path. Caught live, two processes wedged simultaneously:

```
PID 1504 (python3)                      PID 314 (kworker/u16:13)
__switch_to                             __switch_to
msm_gem_new_impl                        rpm_resume
_msm_gem_new                            __pm_runtime_resume
msm_ioctl_gem_new                       recover_worker+0x130
drm_ioctl_kernel                        process_one_work
State: D                                worker_thread
                                        State: D (forever)
```

PID 314 is `recover_worker` (`msm_gpu.c:289`). It takes
`mutex_lock(&dev->struct_mutex)` at line 298, then calls
`pm_runtime_get_sync(&gpu->pdev->dev)` at line 321 **while holding it**, and
wedges in `rpm_resume` forever.

`struct_mutex` is the lock every DRM ioctl needs. So a single hung
`recover_worker` permanently bricks the whole render node: every subsequent
`open`/`ioctl` blocks forever, with **no oops and no kernel record**. That is
the true shape of the "silent death" that this project has been chasing for a
dozen images — not a GPU execution fault, not a PM livelock, a plain
lock-ordering deadlock.

Confirmed live: on the wedged phone, `python3 -c "open('/dev/dri/renderD128')"`
never returns, while `ssh`, `uptime`, and `/proc` reads work fine. Nothing in
dmesg beyond the repeated `HANGCHK141` lines.

## Why `pm_runtime_get_sync` hangs here

Identical to the cause recorded in the image-189 comment at `msm_gpu.c:557`:

> our GPU is powered by the on-demand sysfs trigger, which powers the
> hardware directly and does not drive the runtime-PM state machine.
> `pm_runtime_enable()` was still called at probe, so `pm_runtime_get_sync()`
> believes the device is suspended and tries to resume it — and CAF's resume
> path deadlocks against the state our bring-up left behind.

Consistent with the live evidence: `/sys/.../2c00000.qcom,kgsl-3d0/power/
runtime_status` reads `suspending` — stuck mid-transition, exactly the state
CAF's resume path cannot make progress out of.

Image 189 removed this from `msm_gpu_submit()` and it worked. The same removal
was never applied to `recover_worker()`.

## The fix

`msm_gpu.c`, `recover_worker()`, image 203 → 204 diff is exactly:

```c
-		pm_runtime_get_sync(&gpu->pdev->dev);
 		gpu->funcs->recover(gpu);
-		pm_runtime_put_sync(&gpu->pdev->dev);
```

Upstream v419 (`donor/v419/msm_gpu.c:493`) *does* have these calls under
`struct_mutex`, which is why upstream does not deadlock — but upstream has a
real, functioning runtime-PM suspend path. This tree does not (the GPU is
powered out-of-band by the sysfs trigger), so the same code is fatal here.
`msm_gpu_active()` was checked and is safe (it uses `active_ring`, no PM).

Safe on the same two counts as the image-189 removal: no matching
`pm_runtime_put_*` that matters for correctness, and the GPU is already
powered and clocked, which is all `a6xx_recover()` requires.

## Verification

- Build exit 0, no new warnings.
- Image byte-identical to 203 in header (all 10 fields), cmdline, ramdisk,
  and total size (52543488). Only the kernel differs.

```
artifacts/flash/204-halium-boot-a6xx-nopm.img
sha256 18a222351a53fc4cc7809a8cd7def11e5da5e999109067c01f7fd093d8742c11
```

## Decision rule, declared BEFORE flashing

- **`recover_worker` runs to completion, `hangcheck recover!` appears, the
  render node stays openable across many GL runs** → this was the silent
  death. Promote. The next and final unknown is whether the GPU actually
  *executes* the submitted ring (does `CP_RB_RPTR` advance past the bring-up
  marker, does the a6xx IRQ fire, do fences retire).
- **Render node still wedges** → something else holds `struct_mutex` in D
  forever. Re-run the same `/proc/*/stack` sweep across all D-state tasks and
  find the other holder; the method is proven, just repeat it.
- **`recover_worker` no longer runs at all** → the hangcheck is not firing,
  and the real question becomes why the fence never advances.

## Note on the hangcheck storm

`recover_worker` line 296 does `msm_update_fence(gpu->fctx, fence + 1)` with
`fence = 0`, so `completed_fence = 1`. But the hangcheck's LOCKUP test
(`msm_gpu.c:356`) compares against `gpu->fctx->last_fence` (the *submitted*
counter, 1), not `completed_fence`. So it re-fires every second forever:
`0 < 1` is always true. This is a cosmetic storm, not the cause, but it does
mean recover will be running continuously. Left alone deliberately — one
hypothesis per image, and the hangcheck tuning is not what is blocking us.

## Not changed

Images 187/189/190/191/192/193/195/196/197/198/202/203 all untouched. The
202 coherency fix and the 203 `a6xx_last_fence` fix both stay; 203 is proven
good and this is a strictly additional fix.
