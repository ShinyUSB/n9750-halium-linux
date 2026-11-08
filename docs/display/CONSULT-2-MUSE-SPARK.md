# Consultation 2 — Muse Spark on "freedreno never submits"

Run after image 187, on the question left open by `ROOT-CAUSE-NO-SUBMIT.md`:
the driver's submit path is proven good, so why does freedreno never call it?

## The catch that mattered

I had been treating "freedreno is completely silent" as evidence. It was
worthless, and Muse Spark said so plainly:

> `TU_DEBUG` is a Turnip (Vulkan) variable and does nothing on the Gallium
> path. Use `FD_MESA_DEBUG`.

Verified on the device: `grep -ao "FD_[A-Z_]\{3,\}" msm_dri.so` returns
`FD_MESA_DEBUG`, `FD_BUSY_CYCLES`, `FD_EXT`, `FD_INDEX`, … and no `TU_DEBUG`
path is reachable from GLES. So every "freedreno said nothing under
`TU_DEBUG=bail,sched,sync,ioctl`" observation in images 186-187 was testing a
variable that does not exist on this path. Those conclusions are retracted.

It also corrected a real category error on my part: I had been reasoning about
`tu_flush()` / `fence_batch()` / `tu_dispatch_cmdstream()`, which are **Turnip
(Vulkan)** symbols. The GLES/Gallium path is
`fd_context_flush` → `fd_batch_flush` → `fd_submit_flush` →
`msm_submitqueue_flush` → `DRM_IOCTL_MSM_GEM_SUBMIT`. Different functions,
same destination ioctl, but I had the wrong map.

And it independently reached the same conclusion I did on `SYNCOBJ`: on the
Gallium path, `DRIVER_SYNCOBJ` is not what gates submission (that matters for
Turnip), which is consistent with 187 changing nothing.

## Where I pushed back, and was right to

Muse Spark's primary explanation was "you never flushed — `fd_batch_flush()`
early-returns on `!fd_batch_needs_flush(batch)`, and offscreen FBO + draw with
no flush is normal".

That is **incomplete for our case, and here is the evidence**: `egl8.py`
already calls `glFlush()` and then `glFinish()`, and both return
`glGetError()==0`. If batching were the whole story, `GEM_SUBMIT` would already
be in the trace. It is not — 0 out of 191 ioctls, with `glFlush` returning
success.

So batching is not the answer, but the advice did expose the real gap: I had
no way to observe *when* a submit should have happened relative to the stages
of a GL sequence. Hence image 188.

## Corrected by the same consultation

- **`MAX_FREQ` is not the cause.** I had called `MAX_FREQ=0x283baec0` my
  leading suspect. Muse Spark is right that in Mesa 22.3 `fd_screen_create`
  stores `screen->max_freq` for perf/debug only — device selection is
  `GPU_ID`/`CHIP_ID`, and we already get `FD640`, so selection succeeded. I
  was about to spend a flash cycle on it. **Still a genuine bug worth fixing
  (`base.fast_rate` is unpopulated) but it is not this bug.**
- **No silent "banned" state** that skips work while returning `GL_NO_ERROR`.
  The only latch is `batch->failed` / `context->failed`, which requires a
  *previous failed* submit — and strace proves none ever happened, with
  `MSM_PARAM_FAULTS=0` corroborating. That closes a hypothesis I had listed.
- **The on-demand ordering is not the cause.** If `open`/`GET_PARAM` had run
  before `hw_init`, `fd_screen_create` would have failed and we would be on
  `swrast`, not `FD640` with 134 GL extensions. So the screen latched *after*
  the GPU appeared. This refutes my own question 5 from the previous
  consultation round.

## Net effect

Two of my three live hypotheses are now dead (MAX_FREQ, the banned/ordering
theory), one piece of prior evidence is retracted (TU_DEBUG), and the
remaining question is sharper: **an explicit `glFlush()` returns success and
issues no submit ioctl.** That is the thing image 188 measures directly.

Divergence between the two rounds is worth noting: round 1 both models agreed
and were wrong; round 2 the single consultation was *partly* wrong (batching
explanation) but correctly killed two of my hypotheses and caught a real
methodological error. Adversarial consultation is earning its keep now.