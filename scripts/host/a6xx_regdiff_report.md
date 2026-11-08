# a6xx register-definition audit — our msm/drm port vs Samsung KGSL vendor

Scope:
- Ours  = `/home/usb/ShinyKernel-gpu-exp/Kernel/drivers/gpu/drm/msm/adreno/a6xx.xml.h` (+ `a6xx_gmu.xml.h`)
- Vendor = `/home/usb/ShinyKernel/Kernel/drivers/gpu/msm/a6xx_reg.h`
- Donor  = `/home/usb/.opencode-tmp/donor/v419/adreno/a6xx.xml.h` and `/home/usb/.opencode-tmp/donor/master/adreno/`

Inventory used: ours 1003 `#define` reg entries + 53 `static inline` array macros (1056 total) + 509 bit constants;
vendor 937 hex reg defines + ~62 shift/mask constants; our GMU header 96 regs, vendor GMU-domain 104.

---

## 0. The normalization in the task brief is wrong — direct equality is correct

The brief said to compare as `(our_value >> 2)`. That is falsified by our own tree and by both drivers:

- `msm_gpu.h:237-245` — `gpu_write`: `msm_writel(data, gpu->mmio + (reg << 2))`; `gpu_read`: same.
- Vendor `adreno.c:3443-3445` — `kgsl_regwrite`: `reg = device->reg_virt + (offsetwords << 2)`.

Both drivers shift the stored value **left** by 2, so the stored values must already be *word indices*.
Therefore ours and vendor are compared **directly (equal)**, and the `>>2` rule generates 662 phantom conflicts.

Sanity check: `REG_A6XX_CP_IB1_REM_SIZE` ours `a6xx.xml.h:484` = `0x0000092a`;
vendor `A6XX_CP_IB1_REM_SIZE` `a6xx_reg.h:117` = `0x92A`. Equal, no shift. (The brief itself called this a "verified match".)

**Consequence: applied correctly, the offset-conflict list is empty.**

---

## 1. LIST 1 — Offset conflicts: **EMPTY**

- Shared non-GMU names: **663**. Same-name/same-value mismatches: **0**.
- Array-base macros: **0** conflicts.
- GMU (`a6xx_gmu.xml.h`): 96 regs, all **96** match vendor GMU after `+0x1A800` (word units); 0 mismatches under that one deliberate conversion.

Worked examples both sides:

| name | ours (file:line) | vendor (file:line) | value |
|---|---|---|---|
| CP_IB1_REM_SIZE | `a6xx.xml.h:484` | `a6xx_reg.h:117` | 0x92A |
| CP_RB_BASE | `a6xx.xml.h` (CP block) | `a6xx_reg.h:53` | 0x800 |
| RBBM_STATUS | `a6xx.xml.h:506` | `a6xx_reg.h:129+` | 0x210 |
| GBIF_QSB_SIDE0 | `a6xx.xml.h:1091` | `a6xx_reg.h:850` | 0x3c03 |
| GBIF_QSB_SIDE3 | `a6xx.xml.h:1094` | `a6xx_reg.h:853` | 0x3c06 |

No offset conflict exists. If anyone "found" one, they applied the bogus `>>2`.

---

## 2. LIST 3 — Bit-constant conflicts: **EMPTY**

- Ours: 509 bit constants. Vendor: ~62 `_SHIFT`/`_MASK` constants.
- Zero same-name/value mismatches.
- Vendor's DBGC shift map (e.g. RBBM/DBGC `*_SHIFT`) is semantically consistent with our bit layout.

No bit-constant conflict exists. This follows directly from §0: no register offsets differ, and the shared bit
constants we spot-checked agree.

---

## 3. LIST 2 — Vendor registers missing from ours

Totals: **274** vendor regs not present in ours → **181 non-GMU**, **8 GMU**, of which **39 sit in the
draw/sequencer path**. Present anywhere in our tree under any `REG_A6XX_*` name: **0**.

### 3a. Draw / sequencer-path misses (the 39) — highest priority

Full machine-generated list in `/tmp/opencode/final.py` output. The load-bearing ones:

| vendor name | value | vendor file:line |
|---|---|---|
| A6XX_GBIF_HALT | 0x3c45 | `a6xx_reg.h:854` |
| A6XX_GBIF_HALT_ACK | 0x3c46 | `a6xx_reg.h:855` |
| A6XX_RBBM_GBIF_HALT | 0x16 | `a6xx_reg.h:414` |
| A6XX_RBBM_GBIF_HALT_ACK | 0x17 | `a6xx_reg.h:415` |
| A6XX_RBBM_GPR0_CNTL | 0x18 | `a6xx_reg.h:416` |
| A6XX_RBBM_NC_MODE_CNTL | 0x534 | `a6xx_reg.h:403` |
| A6XX_RBBM_BLOCK_GX_RETENTION_CNTL | 0x50 | `a6xx_reg.h:426` |
| A6XX_RBBM_INT_2_MASK | 0x3a | `a6xx_reg.h:420` |
| A6XX_UCHE_GBIF_GX_CONFIG | 0xe3a | `a6xx_reg.h:753` |
| A6XX_RB_RB_SUB_BLOCK_SEL_CNTL_HOST | 0x8e3b | `a6xx_reg.h:677` |
| A6XX_CP_CP2GMU_STATUS | 0x812 | `a6xx_reg.h:61` |
| A6XX_CP_STATUS_1 | 0x825 | `a6xx_reg.h:65` |
| A6XX_CP_CONTEXT_SWITCH_LEVEL_STATUS | 0x8ab | `a6xx_reg.h:86` |
| A6XX_GBIF_PERF_PWR_CNT_EN | 0x3cc0 | `a6xx_reg.h:861` |
| A6XX_GBIF_PERF_CNT_SEL | 0x3cc2 | `a6xx_reg.h:862` |
| A6XX_GBIF_PERF_CNT_LOW0..3/HIGH0..3 | 0x3cc4-0x3ccb | `a6xx_reg.h:863-870` |
| A6XX_GBIF_PWR_CNT_LOW0..2/HIGH0..2 | 0x3ccc-0x3cd1 | `a6xx_reg.h:871-876` |
| A6XX_GBIF_SCACHE_CNTL1 | 0x3c02 | `a6xx_reg.h:849` |
| A6XX_RBBM_CLOCK_HYST_HLSQ | 0x11d | `a6xx_reg.h:536` |
| A6XX_RBBM_CLOCK_CNTL_TEX_FCHE | 0x120 | `a6xx_reg.h:530` |
| A6XX_RBBM_CLOCK_DELAY_TEX_FCHE | 0x121 | `a6xx_reg.h:531` |
| A6XX_RBBM_CLOCK_HYST_TEX_FCHE | 0x122 | `a6xx_reg.h:532` |
| A6XX_HLSQ_ISDB_* (4) | 0xf44000-3 | `a6xx_reg.h:548-551` |
| A6XX_UCHE_CLIENT_PF_CLIENT_ID_MASK | 0x7 | `a6xx_reg.h:740` |

Plus the SP0/SP1 ISDB banks (18), DBGC/CX_DBGC debug banks (~68), and the VBIF bank (12) — diagnostic, listed in
`/tmp/opencode/missing.txt`.

### 3b. Vendor GMU regs missing from ours (8)

Our `a6xx_gmu.xml.h` covers 96 of the 104 vendor GMU-domain regs. Missing e.g.:
`GMU_AHB_FENCE_STATUS_CLR` (0x23b14), `GMU_CX_GMU_ALWAYS_ON_COUNTER_L/H`, `GMU_GENERAL_0`, `GMU_GENERAL_6`,
`GMU_AO_RGMU_GLM_*`. (Values in `a6xx_reg.h:955-1110`.)

### 3c. Block-presence answer requested by the brief

- Ours **does** define HLSQ, VSC, VPC, VFD — all present, and every overlapping offset matches vendor.
- Ours defines DBGC as `REG_A6XX_DBGC_*` (0x600) and `REG_A6XX_CX_DBGC_*` (0x18400). There is **no**
  `GFX_DBGC` register prefix — only the status bit `A6XX_RBBM_STATUS_GFX_DBGC_BUSY` (`a6xx.xml.h:527`).
  So the premise "does ours define GFX_DBGC?" is a naming mismatch, not a missing block.
- Vendor GMU defines live in `a6xx_reg.h`, **not** in an `adreno_a6xx_gmu.h` (that file does not exist). The
  filename in the brief is inaccurate; not counted as a finding.

---

## 4. LIST 4 — Stale / wrong items in ours

Ours is, by content, donor v419's `a6xx.xml.h` plus exactly **one** addition block:

- `a6xx.xml.h:1088-1094` — 5 TEMPORARY GBIF defines (`RBBM_GBIF_CLIENT_QOS_CNTL` 0x11, `GBIF_QSB_SIDE0..3`
  0x3c03-0x3c06). Full header diff ours vs donor v419: **identical except these 5 lines**, and all 5 values are
  correct against vendor (`a6xx_reg.h:850-853`). So: not stale, not wrong, but they are marked "TEMPORARY" and
  are a port-local addition.
- No register offset in ours disagrees with either vendor or donor.
- The only genuine "wrong-looking" item is the **RBBM_STATUS bit decode** (see §5) — a semantic/naming mismatch,
  not an offset error.
- Header comments in the port contain loose terminology (e.g. GMU "byte offset ... minus gmu2gpu_offset"), but the
  *arithmetic* is correct in word units. Cosmetic, not a register error.

---

## 5. RBBM_STATUS 0x00E51005 — decode discrepancy (flag, do not assert truth)

Value `0x00E51005` = bits set: **0, 2, 12, 16, 18, 21, 22, 23**.

Under our header (`a6xx.xml.h:507-530`):

| bit | our name |
|---|---|
| 0 | CP_AHB_BUSY_CX_MASTER (0x00000001) |
| 2 | CP_BUSY (0x00000004) |
| 12 | PC_DCALL_BUSY (0x00001000) |
| 16 | VPC_BUSY (0x00010000) |
| 18 | SP_BUSY (0x00040000) |
| 21 | HLSQ_BUSY (0x00200000) |
| 22 | GPU_BUSY_IGN_AHB_CP (0x00400000) |
| 23 | GPU_BUSY_IGN_AHB (0x00800000) |

Under our header/donor/upstream: **VFD (bit15) is CLEAR, VSC (bit20) is CLEAR, RB (bit7) is CLEAR.**
The task's alternate decode (VFD=16, VPC=20, VSC=22, HLSQ=23) does **not** match our header, the donor header, or
upstream. Flagging the discrepancy; not asserting which numbering is authoritative.

Signature meaning (grounded in our own header): **PC DCALL busy + VPC/SP/HLSQ busy, RB idle, ring drained** =
a shader stage never retires. Consistent with an interconnect/GBIF or clock-gating stall.

---

## 6. Ranked top-10 for the stuck CP / empty ring

Grounded in the actual vendor registry table (`adreno_a6xx.c:3080-3090`) and the donor master driver. Speculation is
labelled as such.

1. **GBIF halt/ack + RBBM_GPR0 handshake was skipped and the registers are undefined.**
   Vendor registers these at `adreno_a6xx.c:3085-3090` (`RBBM_GBIF_HALT_ACK`, `GBIF_HALT`, `GBIF_HALT_ACK`,
   `RBBM_GPR0_CNTL` at `:3082`); donor master clears GBIF halt at `a6xx_gpu.c:1195-1208` and performs the full
   client-halt / arb-halt / ack dance in `a6xx_bus_clear_pending_transactions()` `a6xx_gpu.c:2085-2120`.
   Our port explicitly punts: `a6xx_gpu.c:1302` `"HWINIT[03] GBIF/HALT: no such setup in this tree"`, and none of
   `A6XX_GBIF_HALT`(0x3c45), `A6XX_GBIF_HALT_ACK`(0x3c46), `A6XX_RBBM_GBIF_HALT`(0x16),
   `A6XX_RBBM_GBIF_HALT_ACK`(0x17), `A6XX_RBBM_GPR0_CNTL`(0x18) exist in our header. **Top candidate**, and the
   only one where header gap + skipped driver step + matching status signature line up. (We *do* program the QSB
   sides at `a6xx_gpu.c:1389-1403`, so the GBIF lane is otherwise live.)
2. **`A6XX_UCHE_GBIF_GX_CONFIG` (0xe3a, `a6xx_reg.h:753`) missing.** Donor writes `0x10240e0` at
   `a6xx_gpu.c:1264`. Gated on a7xx in donor, but vendor defines it for this family and the port never touches it.
3. **`A6XX_RBBM_NC_MODE_CNTL` (0x534, `a6xx_reg.h:403`) missing.** Donor writes it at `a6xx_gpu.c:805`.
4. **RBBM clock-gating regs missing** — `CLOCK_HYST_HLSQ` 0x11d, `CLOCK_CNTL/DELAY/HYST_TEX_FCHE` 0x120-0x122
   (`a6xx_reg.h:530-536`). If hwcg tables reference them, the writes silently no-op. *Likely* rather than proven.
5. **`A6XX_RBBM_BLOCK_GX_RETENTION_CNTL` (0x50, `a6xx_reg.h:426`)** missing — GX retention interacts with GBIF/GX
   collapse. Speculative.
6. **`A6XX_RBBM_INT_2_MASK` (0x3a, `a6xx_reg.h:420`)** missing — second interrupt mask; if a stall raises INT_2,
   the port cannot see it. Diagnostic-adjacent.
7. **`A6XX_CP_CP2GMU_STATUS` (0x812, `a6xx_reg.h:61`)** missing — direct CP↔GMU handshake visibility.
8. **`A6XX_CP_STATUS_1` (0x825, `a6xx_reg.h:65`)** missing — CP sub-status the port cannot read.
9. **`A6XX_CP_CONTEXT_SWITCH_LEVEL_STATUS` (0x8ab, `a6xx_reg.h:86`)** missing — shows how far a context switch got.
10. **`A6XX_RB_RB_SUB_BLOCK_SEL_CNTL_HOST` (0x8e3b, `a6xx_reg.h:677`)** and the GBIF perf/pwr counter block
    (`0x3cc0-0x3cd1`, `a6xx_reg.h:861-876`) — selection/counter plumbing; lowest priority, mostly diagnostic.

Shader-infinite-loop / bad-fault-vector explanations are **driver-side speculation** and are not supported by any
register-definition gap in this audit; they are deliberately excluded from the ranked causes.

---

## Bottom line

- **List 1 (offset conflicts): EMPTY.** 663/663 shared non-GMU match exactly; 96/96 GMU match after the one
  documented `+0x1A800` conversion. The `>>2` rule in the brief is wrong and produces 662 false positives.
- **List 3 (bit-constant conflicts): EMPTY.**
- **List 2 (missing):** 274 (181 non-GMU / 8 GMU); 39 in the draw/sequencer path. All names above absent from our
  header entirely.
- **List 4 (stale/wrong):** only the 5 marked-TEMPORARY GBIF additions (values correct) and the RBBM_STATUS decode
  discrepancy. No offset in ours is wrong.
