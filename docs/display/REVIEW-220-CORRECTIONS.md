# ADVERSARIAL REVIEW — corrections to IMAGE-220-CLEAR-IS-CORRECT.md

Date: 2026-10-11
Reviewer: independent adversarial pass over our own captures

This document **overturns** the headline conclusion of IMAGE-220-CLEAR-IS-CORRECT.md.
Both headline claims were verified wrong against the source and the bytes.

---

## P0 — "The clear is emitted correctly" was a READBACK BLIT, not a clear

`IMAGE-220-CLEAR-IS-CORRECT.md` declared the seq=3 bo[3] stream "a complete,
correct, well-formed 2D blit clear". **It is not a clear.** It is the
`handle_rgba_blit` UBWC→linear decompress from `glReadPixels`.

Three independent falsifiers, all verified:

**1. `SOLID_COLOR` is clear.**
Observed `RB_2D_BLIT_CNTL = 0x10f03000`. In `fd6_blitter.c:278-281`:
```c
uint32_t blit_cntl = A6XX_RB_2D_BLIT_CNTL_MASK(0xf) |
                     A6XX_RB_2D_BLIT_CNTL_COLOR_FORMAT(fmt) |
                     A6XX_RB_2D_BLIT_CNTL_IFMT(ifmt) |
                     COND(color, A6XX_RB_2D_BLIT_CNTL_SOLID_COLOR) |
                     COND(scissor_enable, A6XX_RB_2D_BLIT_CNTL_SCISSOR);
```
`fd6_clear_surface` calls `emit_blit_setup(..., &clear_color, ...)` with a
non-NULL colour, so `SOLID_COLOR` (bit 7, `0x80`) **must** be set. Observed
value is missing exactly `0x80`. A blit with `SOLID_COLOR=0` samples its
source; it does not fill a constant.

**2. Source-sampling state is present; a clear emits none.**
```
dword 17  reg 0x8401 GRAS_2D_SRC_TL_X  cnt=4  0x0, 0x3f00, 0x0, 0x3f00
dword 33  reg 0xb4c0 SP_PS_2D_SRC_INFO cnt=10 0x501330, 0x200040, 0x26000, ...
```
`fd6_clear_surface` emits `GRAS_2D_DST_TL` → `emit_clear_color` →
`emit_blit_setup` → `emit_blit_dst`. **No source rect, no source info.**

**3. `RB_2D_SRC_SOLID_C0` (0x8c2c) is absent from both captures.**
`emit_clear_color` (fd6_blitter.c:734) writes it unconditionally. Verified:
`grep -c 8c2c` → **0** in both `220-run.txt` and `220-gmem.txt`. No
`fd6_clear_surface` exists in any stream we have captured.

**And the addresses settle it:**
```
SP_PS_2D_SRC_INFO  ->  0x26000   (source)
RB_2D_DST_INFO    ->  0x36000   (dest, the linear staging buffer)
```
UBWC→linear. **`0x26000` is the render target as a blit SOURCE** — the exact
same value as `RB_MRT_BASE` in the GMEM run. We treated these as two
inconsistent addresses for the render target; they are one address seen from
opposite ends of a readback.

### Consequence
**We have never captured a clear.** The four images that "found the clear"
(217–220) each decoded a different wrong object. Our decoder identifies a
packet by "has `RB_2D_DST_INFO` + `CP_BLIT`", which is the tail *every* 2D blit
shares.

---

## P1 — `glFramebufferTexture2D` failing was MY HARNESS BUG

```python
glFramebufferTexture2D = proto("glFramebufferTexture2D", None, [ctypes.c_uint]*4)
```

The C signature takes **five** arguments:
`glFramebufferTexture2D(target, attachment, textarget, texture, level)`.
I passed four, so `level` was garbage in `x4` → `GL_INVALID_VALUE`.

This is precisely the ctypes trap already recorded in our own notes. The
entire "genuine state-tracker bug on the render-target binding path" claim, and
the third leg of the "blits don't execute" unification, were **artefacts of my
own test code**. Retracted.

---

## P2/P3 — "readback does not track state" and "blits don't execute" were overstated

The critique correctly identified that "RED then GREEN both black ⇒ readback
broken" is one observation counted twice, and that "clear blit fails AND
readback blit fails" is one observation promoted to a general law about the
2D engine. Both inferences are weak, and "blits don't execute" was circular
given P0.

---

## THE CALIBRATION — now finally valid, and it changes the picture

With the 5-arg fix, the texture FBO now attaches correctly:

```
  fboattach err=0x0 status=0x8cd5 complete=True     <- was 0x501 / INCOMPLETE
  glTexSubImage2D err = 0x0
  ROUND TRIP  want ~(200,100,50,255)  got (0, 0, 0, 0)   READBACK BROKEN
  glClear err = 0x0
  AFTER CLEAR want ~(0,255,0,255)      got (0, 0, 0, 0)   CLEAR DID NOT LAND
```

**This is now a sound measurement.** The framebuffer is COMPLETE, the CPU
upload succeeded with no error, and known data reads back as zeros. Unlike the
previous attempt, this one is not invalidated by an incomplete target.

So `glReadPixels` genuinely does not return what the GPU has. Given the
critique's P0, this is now the **primary** finding, not a secondary one: we
cannot conclude anything about whether the clear executes until readback works.

`FD_MESA_DEBUG=msgs` on the corrected run shows the readback chain in full:
```
fd_blit_from_staging:  linear -> UBWC
handle_rgba_blit:      2d r8g8b8a8_unorm linear -> UBWC
fd_blit_to_staging:    UBWC -> linear
handle_rgba_blit:      2d r8g8b8a8_unorm UBWC  -> linear
fd_clear: 4 64x64 (r8g8b8a8_unorm/none)
handle_rgba_blit:      2d r8g8b8a8_unorm UBWC  -> linear
```
Note the *upload* path (`linear -> UBWC`, i.e. `glTexSubImage2D`) also goes
through `handle_rgba_blit`, and the known pattern does **not** survive it.
So the failure is in the blitter path generally — upload, readback, and
(presumably) clear all share it.

---

## Also corrected: `0x26000` missing from `bos[]` is probably NOT a defect

`IMAGE-220-GMEM-AB.md` called this "the first hard, non-artefactual defect".
It is likely not. `bos[]` lists **BO base addresses**; command-stream
addresses are `bo_iova + bo_offset`. `0x26000` is exactly `0x25000 + 0x1000`,
and `0x25000` **is** a BO in these submits. `0x26000` is very plausibly the
render target's data offset after its UBWC flag buffer. Absence from one
submit's `bos[]` also does not imply unmapped, since mappings persist across
submits.

**Retracted as a defect.** It is more likely an artifact of comparing an
address-with-offset against a list of bases.

---

## Where we actually are

Confirmed:
- Bring-up works; the CP executes the command stream (scratch + fence advance).
- `glReadPixels` does not return GPU content, even for CPU-uploaded known data
  through a COMPLETE framebuffer. **Validated.**
- The blitter path (`handle_rgba_blit`) is exercised by upload, readback and
  (per the code) the clear, and known data does not survive it.
- We have **never** captured a clear, because the stream we decoded was a
  readback decompress.

Unknown:
- Whether the clear executes at all. Unanswerable until readback works.
- Whether the fault is the blitter, UBWC, or something upstream.

## Next — the cheapest discriminating step

`freedreno_screen.c:69-93` confirms both options exist:
`noubwc` (disable UBWC for internal buffers) and `noblit` (skip installing
`ctx->blit`, forcing the generic path).

Run the matrix, one env var each, no rebuild:
```
FD_MESA_DEBUG=noubwc,sysmem
FD_MESA_DEBUG=nobl it,noubwc,sysmem   -> FD_MESA_DEBUG=noblit,noubwc,sysmem
```
- a correct colour appears ⇒ fault is in the UBWC/blitter path, and the two
  runs separate which;
- still black ⇒ fault is upstream of the blitter.

This bisects "blits don't execute" into "blitter broken" vs "something above
the blitter" in one step.

## Method lesson

Five images in a row produced confident findings by identifying packets by a
shared *tail* rather than by their full distinguishing state. The adversarial
pass found it in one. When a decode "looks complete", check the registers that
must be **absent** as well as those that must be present.