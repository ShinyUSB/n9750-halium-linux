# Unoptimizable-draw test: freedreno's batch is genuinely never populated

Kernel `#130`, GPU still loaded from image 188's session (no reboot, no lottery
ticket spent). This is a harness-only test — no kernel change.

## The test

If freedreno were discarding the draw early — no colour attachment, a scissor
rejecting everything, an all-zero colour mask, a zero-area viewport — then the
batch would legitimately be empty and the flush would trivially do nothing.
That is the last explanation standing for "flush succeeds, no submit".

So make the draw impossible to discard:

- colour-attached FBO, `GL_FRAMEBUFFER_COMPLETE` (`0x8cd5`)
- `GL_DEPTH_TEST`, `GL_BLEND`, `GL_CULL_FACE`, `GL_DITHER`, `GL_SCISSOR_TEST`,
  `GL_STENCIL_TEST` all explicitly disabled
- `glColorMask(1,1,1,1)`, full-viewport `glViewport`
- linked GLSL ES 3.00 program, real `glDrawArrays`
- **`glDrawArrays(GL_TRIANGLES, 0, 1000000)`** — one million vertices
- explicit `glFlush()`

## The result: still nothing

```
renderer=b'FD640'   fbo=0x8cd5 (COMPLETE)   prog linked=1
clear err=0x0000
--> glDrawArrays(TRIANGLES, 0, 1000000)
draw err=0x0000
flush err=0x0000
```

and the kernel's view, tagged `huge`, asked for immediately after the flush:

```
CPDUMP[huge] sw : rb_iova=0x1000 rb_size=32768 wptr_dw=99 next_dw=99
CPDUMP[huge] hw : RB_BASE=0x1000 RB_CNTL=0x20c RB_RPTR_ADDR=0x0
CPDUMP[huge] hw : RB_RPTR=0x63 RB_WPTR=0x63 SQE_CNTL=0x1 SQE_BASE=0x9000 (sw 0x9000)
CPDUMP[huge] irq: INT_0_STATUS=0x0 INT_0_MASK=0x3d0e242 CP_INTERRUPT_STATUS=0x0
CPDUMP[huge] st : RBBM_STATUS=0x1 linux_irq=487 irqs=0
```

and strace over the same session:

```
ioctls = 215
MSM_GEM_SUBMIT = 0
SYNCOBJ         = 0
```

**One million vertices, explicitly flushed, `GL_NO_ERROR` at every step, and
`wptr_dw` has not moved off 99.** The batch is genuinely never populated.
Culling/scissor/mask cannot be the explanation.

## Bonus finding: the process segfaults at teardown

Under strace every session ends in `SIGSEGV` *after* all GL work completes and
the harness has printed its final line. Without strace the harness runs to
completion (`HUGE-DRAW-DONE`) and the crash appears on exit. So there are two
separate defects, and it is worth not conflating them:

1. **No submit ever happens** (the blocker).
2. **freedreno segfaults during context teardown** (present even when no
   rendering occurred, so it is not a consequence of #1).

No kernel o accompanies the SIGSEGV — it is purely in userspace.

## Also noted: `drawprobe.py` had a latent bug

The larger multi-variant harness reported `FBO(renderbuffer) status=0x8cd5
err=0x0500` — `GL_INVALID_ENUM` alongside a complete status. That is a real
signature of a bad `glFramebufferRenderbuffer` call. It did not affect the
`v0` baseline reading (`wptr_dw=99`, same as everything else), but it is why I
switched to `hugedraw.py`, which does one thing cleanly instead of five.
Recording it so the multi-variant harness is not trusted again without fixing
that call.

## The honest state of the investigation

Every driver-side explanation is now individually proven wrong:

| checked | result |
|---|---|
| `DRIVER_SYNCOBJ` | enabled; `SYNCOBJ_CREATE` → rc=0 |
| `GEM_NEW` / `GEM_INFO` | 39 + 76 succeed |
| `GEM_SUBMIT` handler | reachable, validates input, returns EINVAL on bad flags |
| ring programming | `CP_RB_BASE == rb_iova == 0x1000`, `RB_CNTL=0x20c`, `SQE_CNTL=1` |
| interrupt mask | bit-exact `A6XX_INT_MASK` incl. `CP_CACHE_FLUSH_TS` |
| CP | alive, +34M per 50ms |
| power / SMMU / allocation | no faults, no failures logged |
| explicit flush | **never results in a submit** |
| unoptimizable draw | **never results in a submit** |

Userspace is clean right up to the submit call:

| checked | result |
|---|---|
| screen create | `FD640`, GLES 3.1, 134 extensions |
| shader compile + link | succeed |
| FBO | `GL_FRAMEBUFFER_COMPLETE` |
| draw calls | `GL_NO_ERROR` |
| kernel errors | **none at all** |

So: the batch is never populated, freedreno never issues `GEM_SUBMIT`, and it
reports success throughout. The fault is inside freedreno's Gallium path
between `fd_context_draw` and `msm_submitqueue_flush`, and it is silent.

## What I would do next, and why it is not obvious

I do not think another kernel image is the right move yet, and I want to be
straight about why. Every remaining hypothesis is *inside Mesa*, and we do not
have Mesa's source on the device — only the shipped `.so`. Guessing further
would be the third round of guessing after the consultants' two wrong answers.

The two things that would actually settle it:

1. **Read the shipped `.so` properly.** `msm_dri.so` is stripped of most
   symbols, but it still contains Mesa's own format strings. Grepping it for
   `TU_DEBUG` (absent) versus `FD_MESA_DEBUG` (present) is exactly how the last
   error was caught. A targeted pass for freedreno's bail/error strings would
   likely name the exact silent path.
2. **A GL limits sanity check.** `GL_MAX_RENDERBUFFER_SIZE=16384` and
   `GL_MAX_VIEWPORT_DIMS=[16384,16384]` are suspiciously round and generic. It
   is worth checking them against known a640 values — if freedreno's screen is
   a degraded or partially-initialised object, that would show up in its limits
   long before it showed up as an error. This costs one harness run, no flash.

Both are free. I would do (2) first because it is one command and could
immediately reclassify the problem, then (1).

Phone alive on `#130`, GPU loaded, reachable over USB RNDIS.