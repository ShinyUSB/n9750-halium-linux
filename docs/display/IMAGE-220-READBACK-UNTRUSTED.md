# IMAGE 220 follow-up — readback does not track GPU state (valid measurement)

Date: 2026-10-11
Image: 220 (no reflash)
Harness: `artifacts/tmp-rescue/harness/rbcheck.py` (new)

## CORRECTION to IMAGE-220-ROUNDTRIP.md

That write-up concluded **"READBACK BROKEN"** from a `glReadPixels` on a
`FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT` framebuffer. **That was not a valid
measurement.** With no valid attachment, `glReadPixels` has nothing to read and
zeros are the expected answer whether or not readback works. The conclusion was
overstated and should not have been drawn from an incomplete FBO.

This harness re-tests using only paths that are demonstrably valid.

## T2 — renderbuffer FBO, two different clears (VALID)

```
  fbo status=0x8cd5 complete=True err=0x0
  clear RED   err=0x0 -> (0, 0, 0, 0)
  clear GREEN err=0x0 -> (0, 0, 0, 0)
  reads identical and not RED -> readback NOT tracking state
```

The framebuffer is **COMPLETE** this time, so this IS a valid measurement.
Two different clears, both `err=0`, both reading back `(0,0,0,0)`.

This is stronger than the clear-failing case, because **readback not tracking
GPU state is a distinct failure from the clear not executing**:

- If the clear failed to run and readback worked, we would read the *previous*
  contents — which after "clear RED" would be RED on the second read, not black.
- We read black for both. Readback is not returning what the GPU has written.

Caveat, stated plainly: `(0,0,0,0)` for both is consistent with EITHER
(a) readback not reading the renderbuffer, or (b) neither clear ever reaching
memory and readback correctly reporting untouched memory. T2 alone cannot fully
separate these — but it does establish that **readback is not a trustworthy
witness**, which is what blocks every conclusion in this project.

## T3 — default framebuffer

```
  default-FB read err=0x506 -> (0, 0, 0, 0)
```

`0x506` = `GL_INVALID_FRAMEBUFFER_OPERATION`. This is the *correct* behaviour
for a surfaceless context with no default framebuffer, and it confirms the
harness itself is sound: it reports errors properly rather than swallowing them.

## Also fixed: GLES2 has no glGetTexImage

Attempting the cleanest possible round trip failed on a real API boundary:
`glGetTexImage` is **desktop GL only**. It is an undefined symbol in
`libGLESv2.so.2` on the device. GLES2 offers no CPU→GPU→CPU texture round trip
that bypasses `glReadPixels` — so the readback path cannot be validated by any
means other than `glReadPixels` itself. That is why this instrument has been
hard to calibrate.

## Where this leaves the investigation

We are **blocked on an uncalibrated instrument**. Every "the clear produced no
pixels" result to date could equally be "the clear worked and readback lies".

The two live, independent defects are unchanged:

1. **Readback does not track GPU state** (T2, valid measurement). The
   `handle_rgba_blit: 2d r8g8b8a8_unorm UBWC -> linear` conversion seen in
   `FD_MESA_DEBUG=msgs` is the prime suspect — it is a GPU blit for the
   compressed→linear readback, and if that blit is not executing, the CPU gets
   zeros while the renderbuffer itself may well contain the clear.
2. **`glFramebufferTexture2D` fails to attach a valid texture**
   (`GL_INVALID_VALUE`, `INCOMPLETE_MISSING_ATTACHMENT`), with Mesa allocating
   `bind=20000a` storage and still not attaching — the render-target binding
   path.

Note the overlap: defect 1's suspected mechanism (a 2D blit not executing) is
exactly the same class of failure as the original problem (a 2D blit clear not
executing). If **blits in general** do not execute on this GPU, that single
fault explains the clear, the readback, and the texture-attach failure
together. That is now the leading unifying hypothesis.

## Next step

Test whether blits execute at all, independent of buffers: if a 2D blit that we
can observe some other way fails too, the fault is "blit execution" rather than
anything about framebuffers. The `RB_MRT` base `0x26000` not being in the BO
list (IMAGE-220-GMEM-AB.md) remains unexplained and should be re-examined under
this hypothesis.