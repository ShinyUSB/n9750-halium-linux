# Image 188 — an explicit glFlush() does not submit. Batching is ruled out.

Kernel `#130`. This closes out the batching hypothesis with direct evidence.

## The experiment

The cpstate probe now takes a caller-supplied tag (`a6xx_gpu_cpstate_store`
copies it into a bounded 32-byte buffer, `strim`s it, and defaults to `"sysfs"`
when empty), so a userspace harness can ask the kernel what the ring looks like
at an exact point in a GL sequence. `harness/flushprobe.py` queries it at five
stages: before any work, after `glClear`+`glDrawArrays`, **after an explicit
`glFlush()`**, after `glFinish()`, and after `glReadPixels`.

## The result

Bring-up won (`ALL 10 PASS`). Three dumps landed, all with the tag mangled to
`[1]` (see "probe bug" below), and every one is identical:

```
[ 118.548676] rb_iova=0x1000 rb_size=32768 wptr_dw=99 next_dw=99
[ 187.667593] rb_iova=0x1000 rb_size=32768 wptr_dw=99 next_dw=99
[ 188.190325] rb_iova=0x1000 rb_size=32768 wptr_dw=99 next_dw=99
```

with `INT_0_STATUS=0x0`, `irqs=0` alongside each.

And strace on the same kernel, same harness:

```
total ioctls: 231
MSM_GEM_SUBMIT: 0
```

**`glFlush()` returns `glGetError()==0` and the ring does not move.** Not one
dword. The last ioctls before the wedge are still `MSM_GEM_NEW` +
`MSM_GEM_INFO` pairs — buffer churn, no submit.

## What this settles

Muse Spark's batching explanation (`fd_batch_flush()` early-returning because
the batch looks empty) is **dead**. `flushprobe.py` calls `glFlush()`
explicitly, it returns success, and 231 ioctls later there is still no submit.
Freedreno is not failing to flush — it is flushing into nothing.

Also confirmed: no kernel-side complaints at all. `dmesg` filtered for
`DRM_ERROR`, msm_bo allocation failures, `ENOMEM`, submit failures — **zero
hits**. The driver is never told to do anything wrong, and never reports doing
anything.

`FD_MESA_DEBUG=flush,draw` produced no output either, so the freedreno build on
this box is either stripped of debug printf or genuinely not entering a path
that logs.

## Probe bug worth recording

The tag came out as `[1]` in all three dumps instead of `[flushprobe]`. Cause:
in `flushprobe.py` I wrote `f.write("1")` to the sysfs node rather than a tag,
and the kernel's new tag-plumbing works correctly — it faithfully printed what
it was given. The kernel side is fine; my harness was passing the wrong thing.
The dumps still worked, because I was reading `wptr_dw` from all `CPDUMP`
lines regardless of tag, so **the measurement is valid even though the
labelling was not**. Both harnesses should write a real tag (`flushprobe\n`)
so per-stage attribution works.

## Where this leaves the investigation

Everything on the driver side is now proven healthy, one item at a time:

| thing | status |
|---|---|
| `DRIVER_SYNCOBJ` | enabled, `SYNCOBJ_CREATE` returns rc=0 (image 187) |
| `GEM_NEW` / `GEM_INFO` | work, 39 + 76 successful |
| `GEM_SUBMIT` driver handler | reachable; validates input, returns EINVAL on bad flags |
| command ring programming | correct: `CP_RB_BASE == rb_iova == 0x1000`, `RB_CNTL=0x20c`, `SQE_CNTL=1` |
| interrupt mask | bit-exact `A6XX_INT_MASK` incl. `CP_CACHE_FLUSH_TS` |
| CP itself | alive, perf counter +35-40M per 50ms |
| GPU power | up, no SMMU faults, no allocation failures |

And on the userspace side:

| thing | status |
|---|---|
| screen creation | succeeds, `FD640`, 134 GL extensions |
| shader compile + link | succeed |
| FBO | `GL_FRAMEBUFFER_COMPLETE` |
| `glClear` / `glDrawArrays` | return `GL_NO_ERROR` |
| **`GEM_SUBMIT` after explicit `glFlush`** | **never issued** |

So the fault is inside freedreno's Gallium flush path, and it is *silent* —
no error, no bail, no kernel error. The remaining candidates, now that
batching, syncobj, MAX_FREQ, banned-state and on-demand-ordering are all dead:

1. **`fd_batch_flush` decides there is nothing to submit.** It early-returns on
   `batch->draw == 0 && !batch->needs_flush`. But we issued a real draw with a
   linked program... unless freedreno never *recorded* the draw, i.e. the draw
   was discarded earlier — in `fd_context_draw` or in the clear/draw-state
   setup, before batching. That would produce exactly this: valid GL calls,
   empty batch, no submit, no error.
2. **A silently-zero capability makes freedreno skip the flush.** Candidates I
   have not yet ruled out: `MSM_BO_UBWC_*` cache-mode flags (hardcoded from DT
   since image 167 — these drive how freedreno sets up tiled buffers and can
   affect whether a flush is considered necessary), and `GMEM_SIZE`/`GMEM_BASE`
   layout assumptions.

Hypothesis 1 is the one I would test next, and it is testable without a new
kernel image: **make the draw trivially unavoidable** — bind a real framebuffer
with a colour attachment the driver cannot optimize away, disable all GL
culling/tiling hints, and try `glDrawArrays` with a much larger vertex count,
then re-check whether `GEM_SUBMIT` appears. If even a 1M-vertex draw produces
no submit, the batch is genuinely never being populated.

The honest summary: after 188 images the GPU has never executed a single
command we submitted, but we have now excluded every driver-side explanation
and confirmed the userspace path works right up to the submit call.

## State

Phone alive on `#130`, GPU loaded, reachable over USB RNDIS. `wptr_dw` remains
the single indicator: the instant it leaves 99, a submit happened.