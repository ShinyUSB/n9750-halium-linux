# RETRACTION — the VBIF halt was NOT the livelock

`ROOT-CAUSE-GLREADPIXELS-LIVELOCK.md` blamed the unbounded `spin_until()` on
`VBIF_XIN_HALT_CTRL1` in `a6xx_pm_suspend()`. **That diagnosis was wrong**, and
the image built from it should not be trusted as a fix.

## The error

`spin_until` is **locally redefined** in this tree, and I missed it:

```c
/* drivers/gpu/drm/msm/adreno/adreno_gpu.h:176-186 */
#define ADRENO_IDLE_TIMEOUT msecs_to_jiffies(1000)

#define spin_until(X) ({                                   \
	int __ret = -ETIMEDOUT;                            \
	unsigned long __t = jiffies + ADRENO_IDLE_TIMEOUT; \
	do {                                               \
		if (X) {                                   \
			__ret = 0;                         \
			break;                             \
		}                                          \
	} while (time_before(jiffies, __t));               \
	__ret;                                             \
})
```

So in `a6xx_gpu.c` and `a6xx_gmu.c` (the latter via `a6xx_gpu.h` →
`adreno_gpu.h`), `spin_until` is a **1-second bounded** macro, not the kernel's
unbounded `spin_until_cond` (`include/linux/processor.h:58-67`). The VBIF poll
therefore returns `-ETIMEDOUT` after one second and continues. It cannot
livelock.

I asserted "unbounded, no timeout, no error path" by reading the *kernel* macro
while the code used the *adreno* one.

## The GBIF/VBIF guard, however, is still correct

Independently of the livelock, upstream gates that exact sequence on
`!a6xx_has_gbif()` (`a6xx_gpu.c:2179`) and a640 is a GBIF part
(`adreno_gpu.h:245-251`, `adreno_is_a630()` false for a640). So halting VBIF on
this hardware is meaningless work on a bus that is not present — it just wastes
1 second per suspend. The fix stands as a **correctness/efficiency** change, not
as a livelock fix. Keep it, but describe it accurately.

## What is still unexplained

The sysfs observation was real and is the part worth keeping:

```
runtime_status = suspending
t1: active=1078944 susp=107514
t2: active=1083957 susp=107514     <- active climbs, suspended frozen
```

`runtime_status=suspending` means `__pm_runtime_suspend()` entered the device
callback and did not return, and the 66 ms autosuspend re-fires forever. That is
still the correct model of the symptom. What is **not** established is where the
callback is stuck, because every `spin_until` on that path is bounded.

So the callback is blocking somewhere that is not a `spin_until`. Candidates not
yet examined:

- `a6xx_gmu_stop()` — the tail of `a6xx_pm_suspend`, after the two bounded polls
- a `wait_for_completion*` / mailbox / IPC to the GMU (`qmp_send`, cmd-db) inside
  the GMU stop sequence
- `clk_disable_unprepare` or an RPMh/SCM call in the CAF suspend tail
- `busy_wait`/`readl_poll_timeout` style loops in `a6xx_gmu.c`

## How to actually find it (not yet done)

The measurement I proposed but did not run: sample `/proc/stat` per-CPU to see
**which CPU** accumulates jiffies, then correlate with the kworker that is in R.
If a single CPU saturates, the callback is spinning (not sleeping) and the stack
will name it. That requires either a working `/proc/<tid>/stack` (this kernel
gives all-zeros for kworkers — `CONFIG_STACKTRACE` may be off for user-readable
stacks) or ftrace.

Cheapest useful step: enable `CONFIG_SCHEDSTATS`/`CONFIG_FTRACE` style tracing,
or add `pr_info` bracketing inside `a6xx_gmu_stop()` — but note the in-tree
warning that a `printk` on the suspend path can wedge the caller inside
`console_unlock` (images 179-181), so that needs care.

## Status

- `ROOT-CAUSE-GLREADPIXELS-LIVELOCK.md` — retracted, see this file.
- Image 245 source currently contains the VBIF guard. It is a correct change
  (matches upstream, saves a wasted second per suspend) but it is **not** a
  livelock fix and must not be described as one.
- The livelock hunt is still open.