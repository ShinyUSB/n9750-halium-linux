# Session 2026-11-11 — pixels are one call away; the wall is a pm_runtime livelock

Kernel `#147` (image 202), unchanged since the flash that fixed the GMU coherency
bug. Read `IMAGE-202-RESULTS.md` first, then `ROOT-CAUSE-GL-READBACK-PM-LIVELOCK.md`.

## State: the GPU is real

Three consecutive boots, all identical and all clean:

```
FORENSIC VERDICT GOOD BABEFACE ms~1
HFI-ACK id=1 / 4 / 3 / 11 / 14 / 10 / 30   (7 messages, 7 ACKs, error=0)
HFI: verify_fw_version ret=0
gpu->funcs->hw_init returned (ret=0)
```

Live probe, right now:

```
CP_RB_CNTL   0x0000020c
sw_rb_iova   0x1000
CP_TIMESTAMP ... delta 676116 clocking 1
```

The boot-time tally in `/root/tally.txt` also works for the first time:

```
boot=ffbb460e resumes=1 babeface=1 baddface=0 timeouts=0
       hfi_ok=1 hfi_to=0 hfi_polls=7 acks=7
       hwinit_ok=1 suppressed=0 smmu_faults=0
       wptr=99 rb_iova=0x1000 clocking=2 verdict=RING_UP
```

## All four liveness conditions confirmed with a live context

`/usr/local/bin/glstage.py`, surfaceless GBM via `EGL_MESA_platform_gbm`:

```
GL_RENDERER = FD640
GL_VERSION  = 4.2 (Compatibility Profile) Mesa 22.3.6
GL_VENDOR   = freedreno
FBO status  = 0x8CD5 COMPLETE
```

## The staged test found exactly where it dies

| stage | call | result |
|---|---|---|
| 1 | `eglInitialize` | PASS |
| 2 | `eglMakeCurrent` | PASS |
| 3 | `glClear` | **PASS** |
| 4 | `glFlush` | **PASS** |
| 5 | `glFinish` | **PASS** |
| 6 | `glReadPixels` | **HARD RESET** |

Real GL work — clear, flush, finish — executes on the A640 without incident.
**`glReadPixels` is the first call that must actually wait for the CP, and it is
the one that kills the system.**

## Cause: pm_runtime livelock, not a GPU fault

From the previous boot's `/proc/last_kmsg` (`captures/death-last_kmsg.log`), RCU
stalls every ~63 s for the entire boot, always the same task:

```
INFO: rcu_preempt detected stalls on CPUs/tasks:
kworker/3:2  R  running task
Workqueue: pm pm_runtime_work
Call trace:
 __switch_to+0x10c/0x120
 pm_runtime_work+0x8c/0xa8
 process_one_work+0x260/0x460
```

23 occurrences: t=82, 85, 102, 146, 165, 209, 211, 228, 272, 335, 398, 461, 524,
587, 650, 842, 861, 902, 905, 924, 965, 968, 987.

The worker is **`R` (running), not `D`** — a livelock, not a deadlock. That is
precisely why there is no crash record: the kernel is alive and logging the whole
time, and the log ends in `UEFI End` (a hard reset) rather than a panic.

Ruled out: zero SMMU faults, zero BADDFACE, zero GMU watchdog, no vblank timeout,
not the 284 s timer.

Related but **not** the same bug as image 189: that was a deadlock in
`msm_gpu_submit()`. This is a livelock in the `pm` workqueue that a GPU readback
triggers.

Suspect found on the live device: **`soc:hwlock@1f40000`** (the GPU power-resource
hwlock) is `control=auto`, `runtime_status=active`, with
`runtime_active_time=167804` / `runtime_suspended_time=4` — held active for
essentially the whole boot. Meanwhile `ae00000.qcom,mdss_mdp` reports
`runtime_active_time=0`, i.e. the display parent never actually resumed. A
hwlock that is never released while the parent is suspended is exactly the shape
that livelocks the `pm` workqueue.

## Next step

Find which resume/suspend callback `pm_runtime_work+0x8c` dispatches into. The
symptom-only frames stop at `__switch_to`, so the real frame is one level deeper
and needs a targeted probe: log the device name on entry/exit of every GPU and
display runtime-PM callback, then read which one never returns.

Do **not** assume this is image 189's bug again, and do not revert the coherency
fix — the bring-up is now 3-for-3 and must stay that way.

## Tooling notes

- `u.sh` now re-assigns the host IP on every call (RNDIS re-enumeration wipes
  it — that was the "100% packet loss", not flakiness). Use `u.sh --fix` to just
  repair the link.
- **Never let an ssh command span a GPU trigger.** It hangs when the reset hits.
  Trigger, detach, poll from the host, read the on-device file.
- `glstage.py` ctypes traps that cost real time: `CDLL.__getattr__` returns a
  fresh `_FuncPtr` each access, so `gl.fn.argtypes = ...` is silently discarded —
  bind to a name first. And without an explicit `restype`, `glGetString`'s 64-bit
  pointer is truncated to 32 bits, segfaulting the *process* in a way that looks
  exactly like a GPU crash.
