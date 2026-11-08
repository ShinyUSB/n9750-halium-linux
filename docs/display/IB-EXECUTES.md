# BREAKTHROUGH — the GPU DOES execute Mesa's IB, and the blit stream is complete

Date: 2026-10-11
Image: 220. No rebuild. All from existing `BOS[]` instrumentation.

## 1. PROVEN: Mesa's indirect buffer executes

`fd6_event_write()` with `timestamp=true` (`fd6_emit.h`):

```c
   OUT_PKT7(ring, CP_EVENT_WRITE, 4);
   OUT_RING(ring, CP_EVENT_WRITE_0_EVENT(evt));
   if (timestamp) {
      struct fd6_context *fd6_ctx = fd6_context(batch->ctx);
      seqno = ++fd6_ctx->seqno;          /* CPU-side counter */
      OUT_RELOC(ring, control_ptr(fd6_ctx, seqno));   /* ADDR_LO/HI */
      OUT_RING(ring, seqno);             /* VALUE -- only the GPU writes this */
   }
```

The seqno *value* is CPU-computed but reaches memory **only** if the GPU
executes the event write.

Observed in `220-bo.txt`, data BOs (flags 0x6003) whose dword 0 increments
exactly once per submit:

```
iova=0x16000:  seq50 = 0x2   seq51 = 0x3   seq52 = 0x4
iova=0x20000:  seq51 = 0x13  seq52 = 0x14
```

And those two addresses are confirmed to be `CP_EVENT_WRITE(timestamp)`
destinations inside Mesa's command streams — scanning every command-stream BO
for `op=0x46, cnt=4` yields targets `0x16000`, `0x1f000`, `0x20000`. The two
incrementing buffers are exactly two of those three.

**⇒ The GPU fetches and executes Mesa's nested indirect buffers, and performs
CP_EVENT_WRITE writes into GPU memory.**

### What this kills

- **"Nested IB fetch is broken" — DEAD.** IBs are fetched and run.
- **"Nothing the GPU is asked to do executes" — DEAD**, as a blanket claim.
- **"No GPU writes ever land" — needs qualifying.** CP_EVENT_WRITE writes land.
  *Data* writes (blit/resolve) do not.

## 2. PROVEN: the 2D blit stream is complete and correct

Register-by-register, the seq=3 blit batch contains everything
`emit_blit_texture()` (`fd6_blitter.c:639-712`) should emit:

| expected | observed | verdict |
|---|---|---|
| `GRAS_2D_SRC_TL_X,4` | dword 17 | ✓ source rect present |
| `GRAS_2D_DST_TL,2` | 0x00000000, 0x003f003f | ✓ exactly 64×64 |
| `RB_2D_BLIT_CNTL` | 0x10f03000 | ✓ IFMT=R2D_UNORM8, MASK=0xf, FMT6_8_8_8_8_UNORM, SOLID_COLOR=0 (correct: this is a texture blit) |
| `GRAS_2D_BLIT_CNTL` | 0x10f03000 | ✓ identical |
| `SP_2D_DST_FORMAT` | 0x0000f180 | ✓ MASK=0xf, COLOR_FORMAT=0x30 |
| `RB_2D_UNKNOWN_8C01` | 0x0 | ✓ |
| `SP_PS_2D_SRC_INFO,10` | 0x501330 | ✓ TILE_MODE=3 (UBWC), FLAGS bit12 set |
| `SP_PS_2D_SRC_FLAGS,6` | present | ✓ UBWC flag plane |
| `RB_2D_DST_INFO,9` | 0x30, 0x36000, 0x0, 0x4 | ✓ TILE_MODE=0 (linear), pitch 4 → 256B = 64px×4B |
| `CP_EVENT_WRITE` event `0x3f` | dword 61 | ✓ `LABEL`, emitted before every CP_BLIT on A6XX |
| `WFI5` | dwords 63, 68 | ✓ both sides |
| **`RB_DBG_ECO_CNTL`** | **0x04100000** | ✓ **exactly the A640 `BLIT_OP_SCALE` workaround** |
| `CP_BLIT` op | 0x3 = `BLIT_OP_SCALE` | ✓ |
| `RB_DBG_ECO_CNTL` after | 0x0 | ✓ |

The blit is textbook-correct, including the one non-obvious register that Emma
Anholt's commit `4ab489a0` showed is specifically required for `BLIT_OP_SCALE`
to work, and which this tree does carry for GPUId(640).

`RB_CCU_CNTL = 0x10000000` is also bit-exact correct:
`ccu_offset_bypass = num_ccu(2) × 64KiB = 0x20000`, encoded
`(0x20000>>12)<<23 = 0x10000000`.

## 3. So what IS broken?

We now have a hard contradiction:

- The CP executes Mesa's IB. ✅ proven by timestamp writes landing.
- The blit command stream is complete and correct. ✅ verified register by register.
- **No data write ever lands.** ❌ render target all zeros, CPU-uploaded pattern
  does not survive, both on the 2D blitter AND on the shader-based
  `util_blitter` render pass.

The natural reading: **the block that performs writes is not operational.**
`CP_EVENT_WRITE` is executed by the CP itself. The 2D blit and the render pass
are executed by the **RB / SP / UCHE** pipeline. Our evidence that "the GPU is
running" has always been CP-only — `CP_SCRATCH_REG(2)` and the ring fence are
both CP-domain. **We have never shown the RB/SP block is clocked or powered.**

This is the one hypothesis that explains every observation:
- CP alive (scratch, fence, event writes) ✓
- IB fetched and executed ✓
- blit stream correct ✓
- nothing written, by either write path ✓
- independent of UBWC, sysmem/gmem, and command shape ✓

## 4. Next: verify RB/SP/GRAS clock and power state

Concretely, at hw_init, read back and log:
- `RBBM_CLOCK_CNTL` (already logged as 0x8aa8aa02 — but that mask may not cover
  every block)
- per-block clock registers if present
- `A6XX_RBBM_GMU_STATUS` / power state
- whether there is a distinct power rail for RB/SP that `a6xx_hw_init` is not
  requesting

Also worth checking upstream: what does mainline `a6xx_hw_init()` write that our
CAF-derived port may skip? A missing clock-enable in the port is exactly the
class of bug that would produce "CP works, everything else silently does
nothing."

This is now a kernel-side investigation and needs a diagnostic image (read-back
of clock/power registers at hw_init, before and after any attempted enable).