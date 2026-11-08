# Image 191 — the submit is ACCEPTED. And the real blocker is now visible.

Date: 2026-10-10. Kernel `#133`. Two results, one a genuine milestone and one
a new lead that explains everything we've been fighting.

## MILESTONE: Mesa's submit is accepted by the kernel (first time ever)

While the GPU was genuinely up (`GL_RENDERER == FD640`, asserted in the same
run), `hugedraw` forced a real submit and strace caught it:

```
ioctls=146
GEM_SUBMIT count=1
1388  ioctl(5, DRM_IOCTL_ETNAVIV_GEM_SUBMIT or DRM_IOCTL_MSM_GEM_SUBMIT,
            0x7f7e89e728) = 0        <-- ACCEPTED (was -1 EINVAL on image 190)
```

This is exactly what image 191 predicted. `MSM_SUBMIT_BO_DUMP` (0x0004) was the
one bit missing from this CAF driver's `MSM_SUBMIT_BO_FLAGS`; with it, Mesa's
`7` (READ|WRITE|DUMP) validates and the submit is handed to `msm_gpu_submit()`.

For the record, the chain now reads:

- image 189 — kernel submit path proven (hand-built submit: `wptr` 99→114, IRQ 0→3)
- image 190 — Mesa enters the submitqueue path at all (DRM version 4 → `sp_funcs`); the NULL-`.flush` crash is gone
- image 191 — **Mesa's submit is accepted (rc=0)**

Whether the GPU then *executes* Mesa's commands is still unproven — see below.

## THE BLOCKER: the GPU comes up, then a second resume times out

The GPU is only alive for ~2.5 seconds. From the on-device capture log, current
boot, filtered to driver lines:

```
[  134.04] POST_START: fw_start end ret=0        <-- GPU COMES UP (our FD640 window)
[  134.04] === GMU BOOTSTRAP BEGIN ===
[  134.05] arm-smmu 2ca0000.kgsl-smmu: Unhandled context fault:
                 iova=0x820, fsr=0x40000402 [TF R SS]
[  134.05] arm-smmu 2ca0000.kgsl-smmu: SOFTWARE TABLE WALK FAILED!
                 Looks like 2ca0000.kgsl-smmu accessed an unmapped address
[  135.55] BC132: ev=RESUME_ENTRY ret=0 depth=2   <-- a SECOND resume, 1.5s later
[  135.55] POST_START: fw_start begin
[  136.70] POST_START: fw_start end ret=-110      <-- second resume TIMES OUT
[  136.70] A6XX_TRACE: a6xx_gmu_resume RETURNING ret=-110
```

So the sequence is: first bring-up genuinely succeeds (this is our FD640
window), the GMU firmware starts bootstrapping, and the KGSL SMMU immediately
throws translation faults. ~1.5 s later something triggers a nested resume, that
resume's `fw_start` times out at `-ETIMEDOUT`, and the GPU goes back down —
after which `get_param` returns `ENXIO` and Mesa silently falls back to
llvmpipe.

## Why we could never catch `wptr` / IRQ movement

The GPU only lives ~2.5 s. Every measurement we attempted over ssh — poke
`cpstate`, read `wptr_dw`, read `/proc/interrupts` — takes longer than that
window, so the GPU is already down by the time we look. This is not a
measurement bug; it is a consequence of the 2.5 s life. Until the resume cycle
is fixed, "did it execute?" cannot be answered from outside.

The SMMU faults are the strongest lead for *why* the GMU doesn't stay up:
`fsr=0x40000402` = translation fault, read, stage-2, with the software table
walk failing — i.e. the GMU firmware is fetching from an IOVA the SMMU can't
resolve. That is upstream of the timeout and would explain both the timeout and
the drop.

## Corrections and honesty notes

- **Retracted:** my earlier "image 191 worked" claim was wrong. A single
  `glClear` is deferred and never submitted, so "no `invalid flags`" proved
  nothing. It took a forced 1M-vertex draw under strace to get a real submit.
  A prior "clean `HUGE-DRAW-DONE`" was actually llvmpipe. Lesson recorded: on
  this project every result must assert `GL_RENDERER==FD640` in the same run.
- `render186.sh`'s `BRINGUP-SUPPRESSED` verdict is **unreliable** — it reported
  "lottery lost" on a boot where `FD640` was actually live. Ground truth is the
  renderer string, never the script's log.
- I could not confirm execution (GPU down before `wptr` could be sampled). The
  honest status is **accepted, not yet proven executed**.

## Next move

Do not chase the submit path further — it is correct now. The blocker is the
GMU bootstrap SMMU fault and the fast follow-on resume timeout. Next step is to
capture the SMMU fault IOVA (`iova=0x820`) and the GMU memory setup, and find
why the second resume is triggered ~1.5 s after a successful bring-up — that
second resume is what actually kills the GPU.