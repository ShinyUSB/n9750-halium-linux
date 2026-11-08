# Matrix result — TWO INDEPENDENT BLIT IMPLEMENTATIONS BOTH PRODUCE NOTHING

Date: 2026-10-11
Image: 220, no rebuild. Env-var matrix on `roundtrip.py` (the FIXED 5-arg version,
so the FBO is COMPLETE and the calibration is valid).

## Matrix

| FD_MESA_DEBUG | fbo | ROUND TRIP | AFTER CLEAR |
|---|---|---|---|
| (none) | COMPLETE | (0,0,0,0) | (0,0,0,0) |
| `noubwc` | COMPLETE | (0,0,0,0) | (0,0,0,0) |
| `noubwc,sysmem` | COMPLETE | (0,0,0,0) | (0,0,0,0) |
| `noblit,noubwc` | COMPLETE | (0,0,0,0) | (0,0,0,0) |

Uniformly black. But the test is only meaningful if the knobs actually changed
behaviour, so both were verified from `FD_MESA_DEBUG=msgs` output:

**`noubwc` works** — blit format changed:
```
handle_rgba_blit: 2d r8g8b8a8_unorm linear -> 2d r8g8b8a8_unorm TILED
handle_rgba_blit: 2d r8g8b8a8_unorm tiled  -> 2d r8g8b8a8_unorm linear
```
(UBWC replaced with TILED, i.e. compression genuinely disabled.)

**`noblit` works and is the important one** — `handle_rgba_blit` disappears
entirely and is replaced by a completely different implementation:
```
fd_blit_from_staging:632
fd_blitter_blit:151: 2d r8g8b8a8_unorm linear -> 2d r8g8b8a8_unorm tiled
fd_blit_to_staging:653
fd_blitter_blit:151: 2d r8g8b8a8_unorm tiled  -> 2d r8g8b8a8_unorm linear
```

## What this eliminates

`fd_blitter_blit` is the **generic `util_blitter` path**, which does a full 3D
render pass with a fragment shader — it does **not** use the 2D blitter or
`CP_BLIT` at all.

So we now have:
- the **2D blitter / `CP_BLIT`** path → black
- the **shader-based render-pass** path → black
- with **UBWC and without** → black
- on **sysmem and gmem** → black

**"2D blits don't execute" is dead.** Two architecturally unrelated
implementations of the same operation both produce nothing. Whatever the fault
is, it is not the blitter, not UBWC, and not the render-target encoding.

This also retires the `handle_rgba_blit` framing from REVIEW-220-CORRECTIONS.md:
the blitter path was a *symptom* location, not a distinct cause.

## What survives

The one thing still standing, and now the most important fact we have:

> `glReadPixels` does not return GPU content, even for CPU-uploaded known data
> through a COMPLETE framebuffer, on two independent code paths.

## The two remaining worlds

The matrix cannot separate these, and I want to be explicit rather than guess:

**(a) No GPU write ever lands.** Nothing the GPU renders or blits reaches memory
at all. Then upload, clear and readback are all equally broken, which is exactly
what we see. Consistent with a broken/missing page-table setup, an unclocked or
unpowered memory-writing block, or the nested-IB fetch hypothesis from the
adversarial review.

**(b) Writes land but readback reads different memory.** The CPU maps one staging
BO while the GPU's blit writes another. Then reads return a freshly-zeroed buffer
— also uniformly black.

Discriminator: check whether the render target buffer's contents change at all,
read by an independent route that does not go through freedreno's blitter — e.g.
dump the BO from the kernel (`BOS[]` instrumentation) *after* a clear, and look
for the colour value in it. If the clear colour is present in GPU memory, the
fault is (b). If the buffer is still zero, it is (a).

That test needs no rebuild: the `BOS[]` dump already prints BO contents, and the
render target is small enough to dump.

## Status of the two retracted claims

- "The clear is emitted correctly" — **retracted** (it was a readback decompress).
- "`0x26000` not in `bos[]` is a hard defect" — **retracted** (base vs offset).

What we can actually assert: the GPU executes the CP (scratch/fence advance),
and nothing we have done makes any GPU-written memory observable.