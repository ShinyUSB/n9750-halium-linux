# The GL reset is a pm_runtime LIVELOCK, and glReadPixels is the trigger

Date: 2026-10-11. Kernel `#147` (image 202). Evidence:
`artifacts/gpu-bringup/captures/death-last_kmsg.log` (2 MB, the previous boot's
complete ring, pulled from `/proc/last_kmsg` after the reset).

## What the staged GL test proved

`/usr/local/bin/glstage.py`, surfaceless GBM, escalating one stage at a time:

| stage | what it does | result |
|---|---|---|
| 1 | `eglInitialize` | **PASS** |
| 2 | + context current | **PASS** |
| 3 | + FBO + `glClear` | **PASS** — `FBO status 0x8CD5 COMPLETE`, survived |
| 4 | + `glFlush` | **PASS** |
| 5 | + `glFinish` | **PASS** |
| 6 | + `glReadPixels` | **SYSTEM RESET** |

With a live context, confirmed on the real chip:

```
GL_RENDERER = FD640
GL_VERSION  = 4.2 (Compatibility Profile) Mesa 22.3.6
GL_VENDOR   = freedreno
```

**So all four liveness-gate conditions are now confirmed on hardware, and real GL
work — including a flush and a finish — executes on the A640 without incident.**

That is a far narrower failure than "the system resets when GL work is
submitted". It specifically dies on **`glReadPixels`**, the first operation that
forces a GPU→CPU synchronisation and therefore the first one that genuinely has to
wait for the CP to retire the work.

## The mechanism: pm_runtime livelock

The dying boot's log is full of RCU stalls, repeating every ~63 s for the entire
life of the boot:

```
[   82.836678] INFO: rcu_preempt detected stalls on CPUs/tasks:
[   82.836740] kworker/3:2  R  running task
[   82.836765] Workqueue: pm pm_runtime_work
[   82.836777] Call trace:
[   82.836793]  __switch_to+0x10c/0x120
[   82.836804]  pm_runtime_work+0x8c/0xa8
[   82.836818]  process_one_work+0x260/0x460
```

Ten or more occurrences: t=82, 85, 102, 146, 165, 209, 211, 228, 272, 335, 398,
461, 524, 587, 650, 842, 861, 902, 905, 924, 965, 968, 987.

Every one is `pm_runtime_work`. This is the **same pm_runtime family as the
deadlock image 189 fixed in `msm_gpu_submit()`** — but on a different path, and
this one is a *livelock*, not a deadlock: the worker is `R` (running), spinning,
never parking. That is why there is no kernel record of a crash and why the phone
simply stops making progress.

`glReadPixels` is what tips it over, because a readback must wait for the GPU to
finish — and waiting is exactly what re-enters the pm_runtime path that is
already spinning.

## What this rules out

- **Not a GPU fault.** Zero SMMU context faults, zero `BADDFACE`, zero GMU
  watchdog bites in this boot.
- **Not a panic.** The log continues normally past every stall and ends in
  `UEFI End` — a hard reset with no kernel-side death record.
- **Not the display vblank class** (`ROOT-CAUSE-284S-RESET-VBLANK.md`). The
  `cont-splash` errors are boot-time only (they stop at t=19.5 s) and no vblank
  timeout appears anywhere in this log.
- **Not the 284 s timer.** This died at a variable time tied to the GL call.

## Where to look next

The stall is in the `pm` workqueue, so the suspects are the runtime-PM
callbacks on the GPU/display power domains — most likely an unbalanced
`pm_runtime_get_sync()`/`put_sync()` or a resume callback that never returns,
reached from the readback path. Concretely:

1. Find what `pm_runtime_work` is calling. The trace only shows
   `__switch_to`, meaning it was descheduled mid-callback, so the real frame is
   one level deeper. `pm_runtime_work+0x8c` is the dispatch point.
2. Check `a6xx_pm_resume()` / `a6xx_pm_suspend()` for a resume path that can
   block — image 202's resume does a **1 second `udelay` poll** for `BABEFACE`
   and further polls for HFI (5 s per message × 8). If a resume is triggered from
   the readback wait, that is seconds of spinning inside a workqueue.
3. Check for reference counting that leaks on the readback path specifically.

**Do not** assume this is the same bug image 189 fixed. That one was a deadlock
in `msm_gpu_submit`; this is a livelock in the pm workqueue that GL readback
triggers. Same subsystem, different failure.

## Capture assets produced

```
/usr/local/bin/glstage.py                    staged GL test, stage N at a time
artifacts/gpu-bringup/captures/death-last_kmsg.log   the boot that died, in full
artifacts/gpu-bringup/captures/t202b.log             the successful bring-up
```

`glstage.py` notes two ctypes traps that cost real time and will bite again:
`CDLL.__getattr__` returns a fresh `_FuncPtr` each access (so `gl.fn.argtypes = …`
assigns onto a temporary and is silently discarded — bind to a name first), and
without an explicit `restype` ctypes truncates the 64-bit `glGetString` pointer,
which segfaults the *process* and looks exactly like a GPU crash.
