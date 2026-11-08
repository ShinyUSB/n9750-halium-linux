# Consultation 3 — Muse Spark, and a real anomaly in the GL limits

## What Muse Spark got right

**The `msm_submitqueue` question was the sharpest thing anyone has asked all
session**, and it produced a *falsifiable prediction I could check against a
trace I already had*: freedreno 22.3 should call `DRM_MSM_SUBMITQUEUE_NEW`
(nr 0x07) once per priority at screen-create. `PRIORITIES=3`, so three calls.

I checked `/var/log/sthuge.txt`, the full-process strace from process start:

```
92  DRM_IOCTL_MSM_GEM_INFO
47  DRM_IOCTL_MSM_GEM_NEW
 9  DRM_IOCTL_MSM_GET_PARAM
```

**Zero `SUBMITQUEUE_NEW`.** So either this Mesa 22.3.6 build does not use the
submitqueue path, or it never got that far. That is a real, concrete new
constraint on the problem — the submitqueue hypothesis is not obviously
exercised here, which weakens (but does not kill) the "NULL queue" theory.

It also correctly noted the init sequence goes
`DRM_IOCTL_VERSION` ×repeated → `GEM_NEW`/`GEM_INFO`/`GET_PARAM`, which is
exactly what the trace shows, with no hardware-register dependency at screen
create. That is consistent with our `fd_screen_create`-then-`hw_init` ordering
not being the problem.

And it correctly killed my "round numbers" lead: `MAX_RENDERBUFFER_SIZE=16384`
and `MAX_VIEWPORT_DIMS=[16384,16384]` are hardcoded `PIPE_CAP` constants in
`freedreno_screen.c`, not probed values. I was about to read a smoking gun
into a constant. High value catch — I had already written those limits into
`RESUME.md` as a "suspicious" signal.

## Where it was wrong or hedged

- **The empty-batch theory is the *same* theory it already retracted.** It
  re-ranks `fd_batch_flush()`'s `!fd_batch_has_draw(batch) && !needs_flush`
  early-out as cause #1, while simultaneously conceding our 1M-vertex
  `glFlush()` test "does not rule out #4/#5". Those are in tension: the
  1M-vertex test is precisely what rules it out, since the only way it still
  no-ops is if the draw never dirtied the batch at all — which is a *different*
  bug (the draw being dropped in `fd_draw_vbo`/emit), not the batching
  early-out. It did name that separately as "draw dropped before it dirtied
  batch", which is the version I think is actually live.
- It explicitly flagged its own uncertainty about the exact 22.3 guard names
  ("less certain on exact `queue==NULL` spelling"), which is honest and useful.

## A genuine anomaly found: the GL limits

I re-ran the limits check with **corrected enums** (my first pass used at
least four wrong constants — `MAX_SAMPLES` as 0x8DA7 instead of 0x8D57,
`MAX_FRAGMENT_UNIFORM_VECTORS` as 0x8B87 instead of 0x8DFD, and two others —
so its "suspicious zeros" were reading adjacent registers). Corrected result:

```
MAX_SAMPLES                     = 4      (a640-accurate)
MAX_COMBINED_TEXTURE_IMAGE_UNITS = 96    (a640-accurate)
MAX_COLOR_ATTACHMENTS           = 8      (correct)
MAX_VERTEX_ATTRIBS              = 16     (correct)
MAX_3D_TEXTURE_SIZE             = 2048   (a640-accurate)

MAX_TEXTURE_IMAGE_UNITS         = 0      *** GLES 3.1 requires >= 16
MAX_COMBINED_UNIFORM_BLOCKS     = 0      *** GLES 3.1 requires >= 12
```

**The screen reports `OpenGL ES 3.1` while violating two GLES 3.1 minimums.**

This is the first hard internal inconsistency we have found in userspace. Most
limits are a640-accurate (`MAX_SAMPLES=4`, `MAX_COMBINED_TEXTURE_IMAGE_UNITS=96`
are exactly right for this part), so the screen is *mostly* correct — which
makes two zeros more suspicious, not less: it looks like partial
initialisation rather than a wrong device match.

**I am not claiming this causes the no-submit.** It is a lead, not a
conclusion. `MAX_TEXTURE_IMAGE_UNITS=0` means fragment shaders cannot sample
textures, which is a shader-compilation constraint, not obviously a
submission one. It may be an independent second bug, or the same root cause
showing up twice. I need to be careful not to do what the last two rounds of
hypotheses did — announce a cause before testing it.

## Current position

Everything remains consistent with: **freedreno never dirties its batch, then
flushes an empty batch and returns success.** The draw is being dropped before
it is recorded — most likely in `fd_draw_vbo` / program-emit / resource
binding, where `GL_FRAMEBUFFER_COMPLETE` is `st/mesa` state and says nothing
about whether a BO is actually bound.

Two concrete next steps, both free:

1. **Bind an actual texture and sample it.** If `MAX_TEXTURE_IMAGE_UNITS=0` is
   real, a fragment shader sampling a texture should *fail to compile* — a
   loud, unambiguous signal that pins down where the screen object went
   wrong. Cheap, one harness run.
2. **Try a draw with no custom shader at all.** If a shaderless
   `glDrawArrays`/`glClear`-only sequence *does* submit, the drop is in
   program emit. If it also does not, the drop is before that.

Neither needs a flash or a lottery ticket.

## Also noted

`gpu_pass=0` on the last check — the bring-up log had rotated out of the
kernel ring by uptime 1281s (known landmine: the ring is small). `wptr_dw` is
still 99, and the machine is alive on `#130`.