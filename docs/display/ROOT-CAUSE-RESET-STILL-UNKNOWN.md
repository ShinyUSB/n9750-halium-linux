# Root cause (corrected): the pm stall is a benign periodic artifact; the reset cause is still unknown

Date: 2026-11-11. Kernel `#147` (image 202).
**Supersedes both** `ROOT-CAUSE-GL-READBACK-PM-LIVELOCK.md` and
`ROOT-CAUSE-PM-STALL-IS-WDT-IPI.md`. Both were wrong; the corrections are
recorded here so neither is re-derived.

## The stall is NOT a livelock

I called it a livelock because the task state is `R`. Measured directly on the
live device while stalls were firing:

```
shell roundtrip ok, delta=0s
dmesg | tail -1  ->  [706.979235] ...      (advancing)
sleep 5
dmesg | tail -1  ->  [712.050148] ...      (still advancing)
RBBM_STATUS 0x00000001
CP_TIMESTAMP ... delta 675999 clocking 1
```

The system is **fully responsive**, the log keeps advancing, and the GPU is still
clocking. A livelock would show none of that. The `R` state is just where the
scheduler happened to sample the task inside `__switch_to`.

## The stall is GPU-correlated but periodic, not triggered

| boot | GPU loaded | GL work | duration | stalls |
|---|---|---|---|---|
| `0403c0c4` | no | no | 982 s (full boot) | **0** |
| `ffbb460e` | yes | no | ~990 s | 23 |

So GPU bring-up enables it. But the timing is machine-regular, not event-driven:

```
stalls:  82.8  102.0  146.2  165.2  209.2  228.3  272.2  291.5  335.3 ...
deltas:   19.2   44.2   19.0   44.1   19.1   43.9   19.2   43.8   19.3 ...
```

An alternating **19.2 s / 44.1 s** pattern, summing to exactly **63.3 s** — which
matches the `t=6303 jiffies` RCU stall period. The driver itself goes quiet at
t=73.9 s and the first stall is t=82.8 s, so it is not triggered by driver
activity either.

It is also **not caused by `glReadPixels`**. The current boot has the GPU up and
has done zero GL work, yet stalls every 63 s. `glstage.py` stages 3-5
(`glClear`/`glFlush`/`glFinish`) all passed because the system was never actually
wedged — only stage 6 died, and that is a separate matter (below).

## The watchdog hypothesis is DEAD

I proposed the apps-watchdog IPI deadlock (images 173-181) because the
`t=6302` jiffie signature matched exactly. It is wrong, on live evidence:

```
cmdline: watchdog_v2.enable=0
lsmod | grep wdt        -> (nothing)
pet_watchdog in dmesg   -> 0 occurrences, ever
Watchdog bark! lines    -> 0 across every log captured, including the death log
```

The Linux watchdog is disabled by our own cmdline and has never run a pet thread
in any of these boots. It cannot be biting. **Do not re-derive this.**

## What the stall actually is

`pm_runtime_work` (`drivers/base/power/runtime.c:857`) holds
`dev->power.lock` — a **spinlock** — across `rpm_resume()`/`rpm_suspend()`:

```c
spin_lock_irq(&dev->power.lock);
...
rpm_resume(dev, RPM_NOWAIT);     /* driver callback runs under the lock */
spin_unlock_irq(&dev->power.lock);
```

The stalled task is always the same one (`kworker/2:0`, CPU 2), always at
`pm_runtime_work+0x8c`, which is inside that callback dispatch. So some device's
runtime-PM callback is slow or blocking, and because the lock is held across it,
RCU sees the CPU as stalled. That is a **latency** problem, not a deadlock, which
matches the system staying responsive.

The device is not the obvious one: `ae00000.qcom,mdss_mdp` is `suspended` with
`runtime_active_time=0` and the GMU is `unsupported` (not runtime-PM managed).
`soc:hwlock@1f40000` is `active` with `runtime_active_time=167804` /
`runtime_suspended_time=4`, i.e. held for essentially the whole boot — the best
remaining candidate, but **not yet proven**.

## Honest status of the reset

The actual hard reset remains unexplained. What is established:

- It is **not** the GPU firmware (bring-up is now 3-for-3 clean, 7/7 ACKs).
- It is **not** a GPU fault (zero SMMU faults, zero BADDFACE, zero GMU WDT).
- It is **not** a display vblank timeout (that class produced a real backtrace;
  nothing similar here).
- It is **not** the Linux watchdog (disabled, never ran).
- It is **not** a kernel panic (log ends in `UEFI End`, no oops, no panic).
- `glReadPixels` is the only call that reproducibly precedes it, but the system
  is *not* wedged beforehand, so it is a load/timing trigger, not a hang.

## Next step

Stop guessing at the reset and get evidence. Two cheap, no-build options:

1. **Bisect the trigger with load, not with calls.** If the reset is
   load-dependent (which the 63 s pattern suggests), run a pure CPU burner on
   the phone *without* touching GL and see whether the phone dies. That
   separates "GPU does it" from "any sustained load does it" in one run.
2. **Get the TrustZone reset reason.** The last log lines before death are
   printed by UEFI (`Exit EBS`, `UEFI End`), and the CAF crash-reporter strings
   (`UploadCause`, `collect_rr_data`) are not in our tree. Reading
   `/sys/fs/pstore` after the reset, or the `sec_debug` partition, is where the
   real cause is recorded.

Option 1 is the better next move: it is a single command, needs no rebuild, and
would immediately tell us whether to keep looking at the GPU at all.

Do **not** revert the image-202 coherency fix. It is proven 3-for-3 and it is
the real bug that was fixed.
