# FINDING: Mesa is exonerated — clear AND draw render-target setup are field-correct

Date: 2026-11-16
Baseline: image 263 (unchanged — no kernel edit, no flash)

## Scope

User instruction: "start with mesa". Specifically, verify whether the **draw**
stream sets up its render target and viewport the same way the **clear** stream
does. This extends the earlier clear-path decode.

## Verdict

**Every render-target and viewport field we can decode is correct, in both the
clear path and the draw path.** Mesa is not the defect. All of the below was
decoded from register definitions found in this tree
(`Kernel/drivers/gpu/drm/msm/adreno/a6xx.xml.h`) or from Mesa's own emitter
source in `/tmp/opencode/mesafull/src/gallium/drivers/freedreno/a6xx/` — not
from recall.

## Method note

Packet header format, **derived from the captured data**, not assumed:

```
0x48 | reg[15:0] | count[3:0] | flag[3:0]      CP_REG_WRITE, `count` values follow
0x40 | reg[15:0] | count[3:0] | flag[3:0]      CP_TYPE_PKT4,   `count` values follow
```

`count` is the low **nibble**; bit 3 is a flag, set on array writes
(e.g. `0x48882286` = `RB_MRT_BUF_INFO` array of 6 dwords). Getting this wrong
initially hid `GRAS_2D_DST_TL` and mis-decoded a pitch — two bugs fixed in
`harnesses/drawpkts.py`.

## 1. Clear path (sysmem / RB_BLIT) — re-verified with field definitions

Register writes decoded from the envelope buffer, against
`fd6_blitter.c:546 emit_blit_dst()` and `fd6_blitter.c:428`:

| Field | Emitted | Definition | Verdict |
|---|---|---|---|
| `GRAS_2D_DST_TL/BR` (0x8405) | `(0,0)`, `(63,63)` | | correct for 64x64 |
| `RB_2D_SRC_SOLID_C0` (0x8c2c) | `255,255,0,255` | | = `glClearColor(1,1,0,1)` |
| `RB_2D_BLIT_CNTL` / `GRAS_2D_BLIT_CNTL` | both `0x10f03080` | | identical, as Mesa intends |
| `RB_2D_DST_INFO[0]` COLOR_FORMAT | `0x30` | `RB6_R8G8B8A8_UNORM = 48` (xml.h) | correct |
| `RB_2D_DST_INFO[0]` TILE_MODE | `0` | `TILE6_LINEAR = 0` (xml.h:98) | correct — RT is linear |
| `RB_2D_DST_INFO[1:2]` | `0x7_00025000` | | = the RT IOVA |
| `RB_2D_DST_INFO[3]` PITCH | `4` | `4 << 6 = 256` | correct = 64 x 4 |
| `RB_DBG_ECO_CNTL` | `0x04100000` then `0x0` | `fd6_draw.c:434-443` | correct blit-enable bracket |
| `RB_2D_UNKNOWN_8C01` | `0` | `fd6_gmem.c:1526`, `fd6_blitter.c:914` | correct — Mesa passes 0 for *all* colour clears |
| `RB_2D_DST_INFO[0]` FLAGS (bit 12) | `0` | `a6xx_2d_surf_info` | correct — this is the UBWC flag; UBWC is off |

### `RB_2D_BLIT_CNTL` — now fully decoded (was the last unverifiable field)

`0x10f03080` against `a6xx_2d_blit_cntl` (Mesa `registers/adreno/a6xx.xml`):

| Field | Bits | Value | Verdict |
|---|---|---|---|
| `ROTATE` | 2:0 | 0 | none |
| `OVERWRITEEN` | 3 | 0 | |
| `SOLID_COLOR` | 7 | **1** | correct — this *is* a solid-colour clear |
| `COLOR_FORMAT` | 15:8 | `0x30` = `RB6_R8G8B8A8_UNORM` | correct |
| `SCISSOR` | 16 | 0 | |
| `MASK` | 23:20 | **`0xF`** | correct — all four channels enabled |
| `IFMT` | 28:24 | `16` = `R2D_UNORM8` | correct — linear, non-sRGB |
| `RASTER_MODE` | 29 | 0 | |

**Every bit is correct for a solid RGBA8 clear.** The field I refused to guess
turned out to be right — and note it needed the real XML to establish, because
the tempting mis-read of `MASK` as bits 3:0 gives 0 (write nothing), which would
have looked like a smoking gun.

## 2. Draw path — render target

The draw cmdstream contains **no address**; on A6xx descriptor state arrives via
`CP_LOAD_STATE6` and the ring-level setup lives in the envelope buffer. Both were
decoded.

Render-target descriptor is `RB_MRT_BUF_INFO` (reg `0x8822`, i.e.
`REG_A6XX_RB_MRT_BUF_INFO(i) = 0x8822 + 0x8*i`), emitted by `fd6_gmem.c:129`
as a 6-dword struct. Observed payload:

```
[0] 0x00002030  RB_MRT_BUF_INFO      COLOR_FORMAT=0x30, COLOR_TILE_MODE=0 (LINEAR), COLOR_SWAP=1
[1] 0x00000004  RB_MRT_PITCH         pitch>>6      -> 4 << 6 = 256 bytes
[2] 0x00000100  RB_MRT_ARRAY_PITCH   array>>6      -> 256 << 6 = 16384 bytes
[3] 0x00025000  RB_MRT_BASE_LO       \
[4] 0x00000007  RB_MRT_BASE_HI        / = 0x700025000  = THE RENDER TARGET
[5] 0x00000000  RB_MRT_BASE_GMEM     0  <- correct: no GMEM base in sysmem mode
```

Register offsets and shifts are all confirmed from `a6xx.xml.h:2873-2913`:

- `REG_A6XX_RB_MRT_BUF_INFO(i) = 0x8822 + 0x8*i`
- `A6XX_RB_MRT_PITCH__SHIFT 0`, macro is `(val >> 6) << SHIFT`
- `A6XX_RB_MRT_ARRAY_PITCH__SHIFT 0`, macro is `(val >> 6) << SHIFT`
- `REG_A6XX_RB_MRT_BASE_LO/HI/GMEM(i) = 0x8825 / 0x8826 / 0x8827 + 0x8*i`

So: correct format, **linear** tile mode, correct base address, pitch exactly
256 B, array pitch exactly 16384 B, GMEM base zeroed as sysmem requires.

## 3. Draw path — viewport / scissor

In the leaf cmdstream (the buffer holding the 11 `CP_DRAW`):

```
[15] GRAS_SC_VIEWPORT_SCISSOR_TL_0 (0x80d0) cnt=2
       TL = 0x00000000 (0,0)
       BR = 0x003f003f (63,63)
```

`0x80d0`/`0x80d1` = `REG_A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0/BR_0`. Correct for
64x64.

## What this leaves

The render backend is handed, in one submission:

- a correctly-addressed **linear** RGBA8 target with the right pitch,
- viewport/scissor covering the full target,
- a clear with the right rect, right colour, right format, right tile mode, and
  the correct `RB_DBG_ECO_CNTL` blit-enable bracket,
- 11 `CP_DRAW`s which the SQE provably executes (`FINDING-SQE-EXECUTES-DRAWS-RB-DEAD.md`),

...and it writes **nothing**. The single remaining asymmetry is unchanged:
CP -> DDR writes work (`memptrs->fence` increments), RB -> DDR writes have never
been observed in any configuration.

## Honest gaps

- `COLOR_SWAP = 1` = `WXYZ` (`adreno_common.xml.h:168-173`). It is Mesa's own
  `fd6_color_swap(pformat, tile_mode)` output via `fd6_pipe2swap()`
  (`fd6_format_table.c:371-385`); the per-format table is generated by
  `freedreno_formats.py`, which is **not** in the local Mesa extract, so the
  value cannot be independently re-derived here. **This cannot explain the
  symptom either way** — colour swap only reorders bytes *within* a pixel that
  is already being written. A wrong swap gives a pixel with wrong channels, not
  an all-zero buffer.
- The draw path's *shader programming* was not field-checked. But it is moot:
  the clear alone should write the RT, and does not.
- Earlier "UBWC ruled out" was **wrong** and is now corrected properly: `noubwc`
  only affects *internal* buffers (`freedreno_screen.c:93`); the RT's UBWC state
  is decided by `freedreno_resource.c:1220-1239`. It is nevertheless not the
  cause here: the harness requests `GBM_BO_USE_LINEAR|SCANOUT` (flags 1|2), and
  `freedreno_resource.c:1229` disallows UBWC for front-buffer rendering. That
  is now corroborated *positively* three ways — `TILE_MODE=0` (LINEAR) in both
  descriptors, `RB_2D_DST_INFO` bit-12 `FLAGS` = 0, and a buffer size of exactly
  16384 = 64x64x4 with no metadata plane.

## Register sources now available locally

Fetched after this pass (the links were correct; only the `/blob/` -> `/raw/`
substitution was missing):

- `/tmp/opencode/regs/mesa-a6xx.xml`, `mesa-pm4.xml` — Mesa 22.3.6, authoritative
  for what the device actually ran
- `/tmp/opencode/regs/kern-a6xx.xml` — current upstream msm registers
- `/tmp/opencode/regs/envy-a6xx.xml`, `envy-pm4.xml` — envytools

**Process lesson:** `/tmp/opencode/mesafull/src/freedreno/registers/adreno/a6xx.xml`
already existed locally this whole time. Earlier passes searched for the
*generated* `a6xx_packets.h` and concluded the definitions were unavailable,
without checking for the XML source they are generated from. Search for the
source spec before concluding a definition does not exist.

## Harnesses added (all in durable path)

- `harnesses/decodeblit.py` — decodes the 2D-blit clear packets
- `harnesses/drawpkts.py` — dumps the full packet stream of every buffer in a submit
- `harnesses/rtiova.py` — identifies the RT BO by size role and traces its IOVA
  through every buffer of the submit, naming the containing register

All three self-check: they report `NOCAP` rather than produce a confident empty
result when the capture is missing, and `drawpkts.py` selects an explicit
`seq` when given one.