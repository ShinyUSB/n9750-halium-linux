# Image 203 — the missing `last_fence` (NULL function pointer in the retire path)

Date: 2026-11-11. Kernel `#148`. Built, packaged, **not yet flashed**.

## The finding

The previous boot's `/proc/last_kmsg` (`captures/reset3-last_kmsg.log`) holds
the first **real oops** this project has ever captured — a kernel page fault
with a full register dump, not a silent reset:

```
[ 2721.661341] Unable to handle kernel paging request
               at virtual address fffffffffffffffc
[ 2721.661664] Internal error: Oops: 96000005 [#1] PREEMPT SMP
[ 2721.661715] Workqueue: msm_drm retire_worker
[ 2721.661734] pc : jopp_springboard_blr_x8+0x4/0x48
[ 2721.661741] lr : retire_worker+0x30/0x74
```

`0xfffffffffffffffc` is a NULL function pointer (minus the prefetch slot),
called through an indirect branch from `retire_worker`.

## Root cause

`msm_gpu.c:521`, `retire_worker()`:

```c
uint32_t fence = gpu->funcs->last_fence(gpu);
```

and our a6xx funcs table (`a6xx_gpu.c:1644`) never provides `last_fence`.
`msm_gpu.h:78` documents the design as *"a6xx provides active_ring instead of
last_fence"* — but three call sites dereference it unconditionally:

```
msm_gpu.c:294  recover_worker()     fence = gpu->funcs->last_fence(gpu)
msm_gpu.c:348  hangcheck_handler()  fence = gpu->funcs->last_fence(gpu)
msm_gpu.c:521  retire_worker()      fence = gpu->funcs->last_fence(gpu)
```

The first of these to run kills the machine. `retire_worker` is queued from
`msm_gpu_retire()`, which runs on the submit-completion path — so the first real
GL submit/retire after bring-up is what dies. That is exactly why bring-up
(which never submits) is 5-for-5 clean while GL work kills the phone.

Worse: `hangcheck_handler()` (`msm_gpu.c:375`) queues `retire_work` on **every
tick** as a *"workaround for missing irq"*. So the crash path also runs
periodically — which explains the resets during idle and clear-only tests, and
why they looked timing-dependent rather than GL-dependent.

This is the same *class* of bug image 190 fixed (`ROOT-CAUSE-DRM-VERSION.md`:
`BLR x4` with `x4 == 0` in userspace `msm_dri.so` from a NULL `.flush`), but in
the kernel retire path. Both are UAPI/funcs-table drift between this CAF-era
driver and the paths that consume it.

## The fix

`a6xx_last_fence()` in `a6xx_gpu.c`, wired into the funcs table. It reads the
fence from the active ring's shadow — exactly the pattern upstream v419 uses
everywhere it needs a fence value (its retire path never calls
`funcs->last_fence` at all):

```
donor/v419/msm_gpu.c:526  fence = ring->memptrs->fence
donor/v419/msm_gpu.c:696  update_fences(gpu, ring, ring->memptrs->fence)
```

```c
static uint32_t a6xx_last_fence(struct msm_gpu *gpu)
{
	struct msm_ringbuffer *ring = a6xx_active_ring(gpu);
	if (!ring || !ring->memptrs)
		return 0;
	return ring->memptrs->fence;
}
```

NULL-safe: no current ring yet → 0 (nothing retired). The hangcheck and recover
paths both tolerate 0 (they compare against stored/submitted fences and only act
on mismatch).

## Verification before flashing

- Build: exit 0, no new warnings (same 3 pre-existing in untouched functions).
- `a6xx_last_fence` present in `vmlinux` (`t a6xx_last_fence`).
- Image: byte-identical to 202 in header (all 10 fields), cmdline, ramdisk
  (`eb5d2eb5…`), total size (52543488). Only the kernel differs.

```
artifacts/flash/203-halium-boot-a6xx-lastfence.img
sha256 39769ce4f961f7d28edebf6205f998c8ca4d7eebc5bffecb497f412750080da4
kernel sha256 324cb3fd3256dae98d9d6ae58b3f9f7cb8aa03c0b9dc14a28cf72158a0b027d1
```

## Decision rule, declared BEFORE flashing

- **Resets stop** (phone survives GL submits, `GEM_SUBMIT` appears, ring
  advances) → `last_fence` was the killer. Promote. Then the remaining work is
  the Mesa batch issue (`ROOT-CAUSE-MESA-NEVER-SUBMITS.md` is still open — zero
  submits have ever reached the kernel).
- **Resets continue with a *different* oops** → read the new oops the same way.
  This method (pstore → pc/lr → caller → missing pointer) just worked once; it
  will work again.
- **Resets continue with NO oops** → the retire path was not the only NULL
  pointer. Audit the remaining funcs-table members against their call sites.

## What this changes about the earlier theories

- The "hard reset on GL submit" was never a GPU execution fault. It was a
  kernel NULL-deref in the retire path, and the resets that "had no kernel
  record" simply never had `last_kmsg` read back. The oops IS the record.
- The `pm_runtime_work` stall (`DECISIVE-CPU-LOAD-TEST.md` proved it harmless)
  and the apps-watchdog theory (`watchdog_v2.enable=0`, never ran) stay closed.
- The Mesa batch issue stays open and is now the critical path: even with the
  retire crash fixed, zero submits have ever reached the kernel, so pixels still
  need the userspace fix. The two bugs are independent and both must land.

## Not changed

Images 187/189/190/191/192/193/195/196/197/198/202 all untouched. The image-202
coherency fix is proven 5-for-5 and stays.
