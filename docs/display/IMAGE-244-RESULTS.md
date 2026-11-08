# Image 244 — a640 hwcg table (TEX_FCHE clock gate); refuted as the cause

Image: `artifacts/flash/244-halium-boot-a6xx-hwcg.img`
sha256 `fdfebc92c349dffa403986b9ebe9d19bbde9bc2afb0f6b47edd41ecec1bca02a`
Kernel `#53`. Booted in ~20 s.

## What changed

CAF's per-chip `a640_hwcg_regs[]` (46 entries) replaces the table that was
actually **a630's** (105 entries). Transcribed verbatim from
`qualcomm-linux/kgsl gfx-kernel.le.0.0 adreno-gpulist.h:1142`, not
hand-typed. Four register defines that were missing from `a6xx.xml.h` entirely
were added from `kernel/msm a6xx_reg.h:530-532,536`:

```
REG_A6XX_RBBM_CLOCK_CNTL_TEX_FCHE   0x00000120
REG_A6XX_RBBM_CLOCK_DELAY_TEX_FCHE  0x00000121
REG_A6XX_RBBM_CLOCK_HYST_TEX_FCHE   0x00000122
REG_A6XX_RBBM_CLOCK_HYST_HLSQ       0x0000011d
```

Measured difference between the two tables:

| | count |
|---|---|
| CAF a640 entries | 46 |
| our (a630) entries | 105 |
| **in CAF a640, missing from ours** | **5** — incl. all three `TEX_FCHE` gates |
| in ours, not in CAF a640 | 63 — `SP1-3`, `RB1-3`, `CCU1-3`, `TP1-3`, `UCHE_CNTL2/3/4` |
| shared, differing values | 7 — mostly the `TP0` hysteresis family (`0x77777777` vs CAF's deliberate `0x00000000`) |

The hypothesis was good: the texture fetch engine is the block that moves
colour data, and its clock gate was never programmed, which fits "RB retires,
zero bytes reach DDR, no fault" precisely.

Also in this image: `a640_qsb` default restored to **on**. Image 243 had turned
`GBIF_QSB_SIDE0..3` off on the strength of mainline's `adreno_is_a610_family()`
scoping, but CAF's `a640_gbif[]` (`adreno_a6xx.c:48-54`) applies it to a640.
CAF is the reference that actually runs on Samsung a640 silicon; I had weighted
mainline too heavily. See `RESEARCH-ASSESSMENT-A640.md`.

## Result: refuted

```
HWCG244: a640 table, 46 entries, state=1 CNTL_TEX_FCHE=0x2
ROQ243:  THRESHOLDS_2=0x02000140
SELFTEST142: done last_iter=10 ALL 10 PASS
HWINIT[25] return ret=0
```

The clock gate is now genuinely programmed (`CNTL_TEX_FCHE` reads back `0x2`,
where before it was never written at all). Bring-up clean, system stable, no
freeze. The cache-sound oracle on a fresh, never-CPU-written buffer:

```
--- round 0: cleared RED ---
  bo 64x64 stride=256 fd=7 inode=37367  FRESH (sound)
  pixel[0,0]     = 0x00000000 (r=0 g=0 b=0 a=0)
  nonzero pixels = 0 / 4096
--- Test C --- READ PATH OK
```

Still all zeros. **The hwcg table was not the missing write.** The fix stands
on its own merit — programming the correct per-chip table is right, and it
fixed a real latent bug (three phantom SP/RB/CCU banks and an unclocked TEX_FCHE)
— but it does not explain the symptom.

## New observation from this image's ibdump

Fresh `ibdump` of three submits shows **five BOs per submit**, and the one with
distinct flags is not the first:

```
BOS[seq=17] bo[0] iova=0x700025000 flags=0x6003
BOS[seq=17] bo[1] iova=0x700021000 flags=0x6003
BOS[seq=17] bo[2] iova=0x700022000 flags=0x6003
BOS[seq=17] bo[3] iova=0x700016000 flags=0x6003
BOS[seq=17] bo[4] iova=0x700029000 flags=0x6007   <- distinct flags
```

`bo[4]` is the only one with `flags=0x6007` (others `0x6003`), and it
**alternates between `0x700029000` and `0x70002a000`** across submits — the
behaviour of a rotating front buffer. This is the strongest candidate yet for
"which BO does the GPU actually render into", and it is directly checkable: add
a print of `flags` to the oracle's dmabuf so we can match the locked front
buffer's IOVA and flags against this list, and confirm the `CP_BLIT` destination
really is `bo[4]`.

Worth noting the IOVAs are tightly clustered — `0x700016000` through
`0x70002a000`, ~84 KB — for what should be a 16 KB RGBA8888 buffer plus command
stream. That clustering should be explained rather than assumed.

## Cumulative refutation list

Everything reachable from `hw_init` is now tested or applied and does not fix
the render:

| item | image | result |
|---|---|---|
| LLC / SCID / slice activation / domain attr / map hint | 242 | refuted |
| UBWC amsbc, `RBBM_NC_MODE_CNTL` | 237, 244 | refuted |
| A640 clock values, `RBBM_CLOCK_CNTL` | 235 | refuted |
| `LLC_NWA` + `NOEXEC` | 236 | harmful, reverted |
| `CP_ROQ_THRESHOLDS_2` a640 value | 244 | refuted |
| GMU AO CGC + `WFI_CONFIG` | 244 | refuted |
| `GBIF_HALT` clear | 244 | **freezes the machine** (A/B) |
| a640 hwcg table / `TEX_FCHE` gate | 244 | refuted |
| `GBIF_QSB_SIDE0..3` | 242 on / 244 on | no change either way |
| SMMU identity mapping | retracted | not the defect |
| `CP_SMMU_TABLE_UPDATE` | retracted | v5.10+ only |
| `RB_DBG_ECO_CNTL` blit magic | research | already correct in our stream |
| `UCHE_GMEM_RANGE_MIN/MAX` | research | correct, both halves via `gpu_write64` |
| `sp_local_gpuaddr` / `sp_pvt_gpuaddr` | research | mainline omits these too |

## Recommended next steps, in order

1. **Identify the actual render target.** Print the locked dmabuf's IOVA and
   flags from the oracle and match against the `ibdump` list. Confirm the
   `CP_BLIT` destination is `bo[4]` (flags `0x6007`). This is cheap and closes a
   real gap: we currently assume the buffer we read is the buffer the GPU wrote.
2. **Fix `glReadPixels`.** It is the one instrument that could distinguish
   "render works, final blit doesn't land" from "render never happens", and it
   is broken by the `pm_runtime` livelock. `EGL_EXT_image_dma_buf_import` is
   absent on this stack, so the swapchain dmabuf is the only surface we can
   read — which is why step 1 matters and why this blind spot exists.
3. **Zap / secure mode.** Firmware is in hand (`GPU-ZAP-FIRMWARE.md`); needs a
   DT `zap-shader` node plus reserved memory, and a driver port of the a5xx
   loader. Still carries the caveat that the fence echo works, so a blanket
   secure-mode stall does not by itself explain CP-writes/RB-doesn't.

## State

Phone is up on kernel `#53` with all six toggles at their intended defaults
(`a640_gbif_halt=N`, everything else `Y`). Stable, bring-up clean.