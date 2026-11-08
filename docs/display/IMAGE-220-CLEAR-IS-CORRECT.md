# IMAGE 220 — THE CLEAR IS EMITTED CORRECTLY. The GPU IS NOT EXECUTING IT.

Date: 2026-10-11
Image: `artifacts/flash/220-halium-boot-a6xx-bos.img`
Capture: `artifacts/gpu-bringup/captures/220-run.txt` (12615 lines)

## The finding

After four images chasing the wrong buffer, image 220's `BOS[]` dump finally
captured the real command stream. **`bo[3]` of `seq=3` (iova `0x2c000`, the
93-dword nested IB) contains a complete, correct, well-formed 2D blit clear:**

```
 22 PKT4 GRAS_2D_DST_TL(0x8405)   = 0x00000000, 0x003f003f   <- 0..63 = 64x64
 25 PKT4 RB_2D_BLIT_CNTL(0x8c00)  = 0x10f03000
 27 PKT4 GRAS_2D_BLIT_CNTL(0x8400)= 0x10f03000
 29 PKT4 SP_2D_DST_FORMAT(0xacc0) = 0x0000f180
 51 PKT4 RB_2D_DST_INFO(0x8c17) cnt=9
      0x00000030, 0x00036000, 0x00000000, 0x00000004, ...
 66 PKT7 CP_BLIT(0x2c)            = 0x00000003               <- BLIT_OP_SCALE
```

Cross-checks, all consistent:

- `GRAS_2D_DST_BR = 0x003f003f` is exactly a **64x64** destination.
- `RB_2D_DST_INFO` destination address = **`0x36000`**, which is
  `BOS[seq=3] bo[2] iova=0x36000` — **the render target BO.**
- `CP_BLIT = 0x2c` confirmed against `adreno_pm4.xml`
  (`<value name="CP_BLIT" value="0x2c" variants="A5XX-"/>`), payload `0x3`
  = `CP_BLIT_0_OP(BLIT_OP_SCALE)`.
- This is `fd6_clear_surface()` (fd6_blitter.c:795-840) firing exactly as
  written, called from `emit_sysmem_clears()`.

**Mesa is doing everything right.** The colour target is bound, the scissor is
right, the blit is issued, the destination is the render target.

## What this rules out

Everything Mesa-side is now exonerated:

- NOT an empty `batch->framebuffer` (217's claim — a stale-capture artefact)
- NOT `nr_cbufs == 0` / missing `RB_MRT_BUF_INFO` (218/219 claims — also
  artefacts; the clear went down the **sysmem/blitter** path, which never uses
  `RB_MRT_BUF_INFO` at all)
- NOT a missing render target, missing scissor, or malformed commands
- NOT the UBWC/linear readback blit

The failure is **GPU-side**: the command stream is correct and is being handed
to the right address, and the GPU is not performing the write.

## The new question

The IB at `0x2c000` is referenced by `CP_INDIRECT_BUFFER` from `cmd[0]`
(seq=1) — but note the submit ordering: seq=3's own `cmd[0]` references
`0x2c000` at **93** dwords where seq=1 referenced it at **77**. The same buffer
grows between submits, so it is genuinely accumulating/reusing storage.

The decisive next question is now narrow and testable: **does the GPU fetch and
execute the nested IB at all?**

Concretely, three candidates, in order:

1. **Nested IB fetch fails.** The outer `cmd[0]` issues
   `CP_INDIRECT_BUFFER iova=0x2c000, 77 dwords`. If the CP cannot fetch a
   nested IB (page fault, SMMU, or an SQE/IB2 configuration issue on this
   CAF-format blob), the blit is silently skipped — which matches a correct
   stream with no effect *exactly*. This is now the leading hypothesis.
2. **`CP_SKIP_IB2_ENABLE_GLOBAL` / `CP_SKIP_IB2_ENABLE_LOCAL`** are present in
   the outer stream (op 0x1d). If IB2 skipping is misconfigured, the CP may
   skip executing IB2 payloads it should run.
3. The blit executes but `RB_2D_DST_INFO` pitch/format (`0x00000030`,
   `0x00000004`) targets something unexpected.

The cheap discriminator for (1) and (2): compare a submit that inlines the
clear into the outer buffer versus one that nests it. If the GMEM path (which
emits the clear into `batch->gmem` directly, no nesting) works where the
sysmem path does not, nested IB fetch is confirmed as the fault.

## Method note

Four consecutive images (217-220) each missed by one level of indirection:
ring -> cmd[] -> IB_TARGET_BUF -> bos[]. Two produced confident but entirely
wrong conclusions. The thing that finally worked was dumping the *whole*
submit structure rather than reasoning about which part "should" hold the
data. Worth remembering before the next "the driver must be broken" moment.