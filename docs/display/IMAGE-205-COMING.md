# Image 205 — stop `a6xx_recover` from power-cycling the GPU

Date: 2026-11-11. Built, packaged, **not yet flashed**. One change, in
`a6xx_recover()`.

## Image 204 result: PROMOTED, and it was the big one

204 removed `pm_runtime_get_sync()` from `recover_worker`. Verified on hardware:

| check | result |
|---|---|
| clean bring-up | `BABEFACE ms~1`, 7/7 HFI ACKs, `hw_init ret=0`, CP clocking |
| `renderD128` open/close | OK |
| `glstage.py 5` ×5 consecutive | **5/5 PASS** |
| oops since flash | **0** |
| PID 314 `rpm_resume` D-state | **gone** |

Before 204 the render node was bricked by one hung `recover_worker` holding
`struct_mutex`. After 204 it survives repeated GL runs. That deadlock was the
"silent death" this project chased for a dozen images.

## Two corrections to the record

**1. Mesa was always submitting.** `dmesg | grep -c GEM_SUBMIT` returning 0 was
a grep for a string that does not exist in this kernel (only in comments). The
real evidence:

```
HANGCHK141: enter fence=0 stored=0 submitted=5
```

`submitted` is `gpu->fctx->last_fence`, incremented in exactly one place —
`msm_fence_alloc()` (`msm_fence.c:154`) from `prepare_submit()`
(`msm_gem_submit.c:537`). So **5 submits reached the kernel** and ran through
`a6xx_submit()`, writing to the ring.

This retires `ROOT-CAUSE-MESA-NEVER-SUBMITS.md` and `CLEAR-DRAW-BOTH-SWALLOWED.md`.

**2. There is no `ring->seqno` to compare against.** Upstream v419's hangcheck
tests `fence < ring->seqno`. In this tree `ring->seqno` is initialised to 0 at
`msm_ringbuffer.c:39` and never incremented anywhere, so that comparison cannot
be used as-is.

## The real remaining bug, as measured on 204

```
t=69.8      CPDUMP auto-rest: MMIO register reads SUCCEED
            RB_RPTR=0x63 RB_WPTR=0x63 RB_CNTL=0x20c irqs=0
t=100-118   five glstage runs PASS
t=118.3     HANGCHK141 LOCKUP fence=0 submitted=5 -> recover_worker enter
t=130+      a6xx_gpu_cpstate_show permanently D, returns 0 bytes
t=700+      glstage HANGS AT STARTUP (was 5/5 passing minutes earlier)
```

`a6xx_recover()` ran exactly once and it did this:

```c
gpu->funcs->pm_suspend(gpu);
gpu->funcs->pm_resume(gpu);
msm_gpu_hw_init(gpu);
```

After it, **MMIO never returns again**. The cpstate reader wedges inside its
register reads and EGL init blocks — an ioremap read against a power domain
this tree left half-transitioned has no timeout and no watchdog, so it is D
forever with nothing in dmesg.

This is the same mismatch as image 203/204: the GPU is powered by the on-demand
sysfs trigger, bypassing runtime PM, so driving `pm_suspend`/`pm_resume` from
the driver moves a state machine the hardware was never put into. Upstream v419
does the same thing here, but upstream has a real suspend path.

Why it matters: it fires after a single *spurious* hangcheck, and it destroys
the only instrument available for answering the one question left — **does the
GPU actually execute the ring?**

## The fix

Keep the dumps (pure reads, and the reason recover is called), drop the reset:

```c
-	gpu->funcs->pm_suspend(gpu);
-	gpu->funcs->pm_resume(gpu);
-	msm_gpu_hw_init(gpu);
```

If the GPU genuinely hangs, the answer is a fix to the hang, not a power cycle
that also takes out register access.

## What 205 will settle

With the power cycle gone, register reads survive, so for the first time we can
observe a submit directly:

| observation | conclusion |
|---|---|
| `CP_RB_RPTR` advances past `0x63` after a submit | GPU executes; bug is fence accounting only |
| `CP_RB_RPTR` stays at `0x63` | GPU never consumes the ring — real execution failure |
| IRQ 487 (`A640`) count rises per submit | per-submit interrupt path is live |
| `CP_SCRATCH_REG2` ≠ 0 in the recover dump | `a6xx_submit()` got past its scratch write |

Note on 204's numbers: IRQ 487 `A640` read **5**, matching `submitted=5`, and
stayed at 5 across a further run. That is suggestive but **not** proof — the
later GL run hung and may never have submitted. 205 removes the confound.

## Secondary candidate, not yet acted on

`adreno_gpu->memptrs` is allocated `MSM_BO_UNCACHED`
(`adreno/adreno_gpu.c:595`), which maps `pgprot_noncached()`. The driver itself
says these buffers are *"not coherent"* with the GPU (`msm_gem.c:179-182`) and
only calls `sync_for_cpu()` on free — never on the read path. So even if the
GPU writes the fence, `a6xx_last_fence()` reading `ring->memptrs->fence` may
never observe it. This is the mirror image of the image-202 coherency bug.

Deliberately **not** fixed in 205. Whether it is even the problem depends on the
RPTR result above, and one hypothesis per image.

## Verification

- Build exit 0, no new warnings.
- Byte-identical to 204 in header (all 10 fields), cmdline, ramdisk, size.

```
artifacts/flash/205-halium-boot-a6xx-norecover.img
sha256 e3c5dbf39feb883cb41e3b4972a9c184a1f04cec0539ac359c955cf4c05c8227
```

## Not changed

Images 187–204 all untouched. 202 (coherency), 203 (`a6xx_last_fence`) and 204
(recover pm_runtime) are all proven and stay.
