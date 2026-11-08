# 🎯 ROOT CAUSE #2 FOUND — `msm_gpu_submit()` deadlocks in `rpm_resume`

Caught live on `#130` with a hand-built submit, on 2026-10-09. The stack is
unambiguous and this is the first *proven* driver-side blocker.

## The evidence

A fully valid `GEM_SUBMIT` — one that passed every validation in
`msm_ioctl_gem_submit()`, including BO pinning and iova allocation — hung:

```
PID 2511, State: D (disk sleep)
syscall: 29 0x3 0xc0286646 0x7fa1693750 ...

[<0000000000000000>] __switch_to+0x10c/0x120
[<0000000000000000>] rpm_resume+0x280/0x78c
[<0000000000000000>] __pm_runtime_resume+0x54/0x80
[<0000000000000000>] msm_gpu_submit+0x5c/0x1b8
[<0000000000000000>] msm_ioctl_gem_submit+0xc14/0xd50
[<0000000000000000>] drm_ioctl_kernel+0x110/0x134
[<0000000000000000>] drm_ioctl+0x2cc/0x520
[<0000000000000000>] do_vfs_ioctl+0x6bc/0xa7c
[<0000000000000000>] SyS_ioctl+0x90/0xa0
```

`0xc0286646` is `DRM_IOCTL_MSM_GEM_SUBMIT` (verified against two proven-good
request numbers). The process had to be `kill -9`'d.

The offending code, `msm_gpu.c:539`:

```c
void msm_gpu_submit(struct msm_gpu *gpu, struct msm_gem_submit *submit,
		struct msm_file_private *ctx)
{
	WARN_ON(!mutex_is_locked(&dev->struct_mutex));

	pm_runtime_get_sync(&gpu->pdev->dev);   /* <-- D, forever */

	msm_gpu_hw_init(gpu);
	...
```

## Cause

This GPU is powered by the **on-demand sysfs trigger**, which powers the
hardware directly and does not drive the runtime-PM state machine.
`pm_runtime_enable(dev)` was still called at probe (`msm_drv.c:706`), so
`pm_runtime_get_sync()` believes the device is suspended, calls resume, and
CAF's resume path deadlocks against the state our bring-up left behind.

## This is image 184's bug, one function over

`adreno_get_param()` had the *identical* `pm_runtime_get_sync()` wrapped around
the timestamp read, and wedged python3 in D for minutes. We removed it in
image 184; the log then showed `a6xx: timestamp: GPU_SET OOB ret=-110, reading
anyway` and the wedge was gone.

We never applied the same remedy to the **submit** path, because until this
moment no submit had ever been attempted and there was no evidence to justify
touching it. Two functions, same anti-pattern, one fixed by accident.

## Two secondary findings from the same investigation

**The submitqueue hypothesis is dead.** Muse Spark's best idea in round 3 was
that freedreno needs `DRM_MSM_SUBMITQUEUE_NEW`. Reading the driver settles it:

- `msm_ioctl_gem_submit()` **never references `args->queueid` at all.** The
  field is parsed and ignored. Whatever `queueid` userspace sends, the kernel
  behaves identically. So a wrong/NULL/stale queue id cannot block a submit
  here — the feature simply does not exist in this CAF driver.
- `DRM_MSM_SUBMITQUEUE_NEW` is `0x0A`, not `0x07` as Muse Spark stated.
- `msm_ioctl_submitqueue_new()` is a stub that ignores its input and hardcodes
  `args->id = 1`.

So the teardown SIGSEGV is *not* explained by a submitqueue use-after-free,
which was my best guess an hour ago. That guess is retracted.

**A driver bug found by reading, not by testing.** `pm_runtime_get_sync()` in
`msm_gpu_submit()` has **no matching `pm_runtime_put_*` anywhere in the
function**. The reference was never balanced. That is independent evidence the
call was not load-bearing as written, and it makes removing it safe rather than
a new leak.

## What this does NOT explain

Honesty, because it matters for what comes next:

freedreno still issues **zero** `GEM_SUBMIT` ioctls — verified by strace over
215 ioctls including a 1M-vertex draw with an explicit `glFlush()`. That is a
*different* bug from this one. This bug means that **if** freedreno ever did
submit, it would wedge in D forever.

So there are at least two independent defects:

1. **Driver (this one, now fixed in 189):** `msm_gpu_submit()` wedges in
   `rpm_resume` → no submit could ever complete, ever.
2. **Userspace (still open):** freedreno never issues the ioctl at all.

Fixing #1 removes a hard blocker but will not by itself produce pixels. I am
not going to claim otherwise this time.

## The fix (image 189)

Remove the `pm_runtime_get_sync()` from `msm_gpu_submit()`, with a comment
recording the stack above. `msm_gpu_hw_init()` stays and is what actually
needs the power. Safe because:

- there is no matching put to unbalance, and
- `hw_init` already tolerates the GPU being up (`needs_hw_init` is cleared
  after the on-demand bring-up, so it no-ops).

Same remedy, same reasoning, same precedent as image 184.

Harness: `harness/submitqueue.py` (also the tool that caught this — it uses
the ioctl encoder with two proven-good regression assertions).