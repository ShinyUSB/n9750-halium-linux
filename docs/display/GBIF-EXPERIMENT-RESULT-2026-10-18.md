# Image 268 — GBIF experiment result (clean NEGATIVE), and an upstream init screen

Date: 2026-10-18

## Part 1 — the GBIF experiment

### Result: HYPOTHESIS B ELIMINATED, with the falsifiability ladder satisfied

| level | check | result |
|---|---|---|
| 1 | `strings` on the packed kernel — `HWINIT[27]` WRITTEN branch present | **PASS** |
| 2 | `/proc/cmdline` contains the complete `a6xx.uche_gbif_rw=1` token | **PASS** (token 11, not truncated) |
| 2 | `swiotlb=2048` preserved / `androidboot.memcg` removed | **PASS** |
| 3 | `HWINIT[27] UCHE_GBIF_GX_CONFIG=0x10200f9 WRITTEN (a6xx.uche_gbif_rw=1)` | **PASS** |
| 4 | workload exercised the relevant path | **PASS** |

Bring-up was clean: `SELFTEST142: done last_iter=10 ALL 10 PASS`, `HWINIT[25] return ret=0`.

Workload results, oracle-validated (unchanged harness, unchanged workload):

| test | prefill intact | expected pattern | verdict |
|---|---|---|---|
| T1 compute → SSBO (`NDISP=64`) | **256/256 = 0xAA** | 0/64 | **GPU wrote nothing** |
| T2 vertex → transform feedback | **1024/1024 = 0x5A** | 0/64 | **GPU wrote nothing** |
| render `pixelreadback.py` | — | **0/6 checks** | **all zeros** |

`CP_INTERRUPT_STATUS = 0x00000000`, `RBBM_STATUS = 0x00000001` — no faults.

Because levels 1–4 all pass, this is an **interpretable** negative: writing
`UCHE_GBIF_GX_CONFIG = 0x10200F9` (the KGSL value) does **not** change behaviour.
Candidate B is eliminated, not merely unproven.

Image flashed: `268-halium-boot-a6xx-uche-gbif.img`,
sha256 `5e79a9f91c04232818b19f4f5bd75bcb683e1bfa560180d072dc830fe605d3ac`.

Note: the earlier analysis rated B as "high priority". That rating assumed the
parameter was functional. It was not — `uche_gbif_rw` had no conditional write
anywhere. Had this been flashed before implementing it, the null result would
have been misread as eliminating GBIF.

---

## Part 2 — two hypotheses raised and killed during the same pass

### 2a. `*_ADDR_MODE_CNTL` — KGSL and upstream genuinely disagree; **we match upstream**

Our tree writes `0x1` to twelve `ADDR_MODE_CNTL` registers
(`a6xx_gpu.c:2450-2473`, the "ADDR64" block from image 128).
KGSL's `a6xx_pwrup_reglist[]` writes **`0x0`** to all of them.

Checked against **upstream Linux** (`a6xx_gpu.c:1223-1237`):

```c
if (!adreno_is_a7xx(adreno_gpu)) {
        /* Turn on 64 bit addressing for all blocks */
        gpu_write(gpu, REG_A6XX_CP_ADDR_MODE_CNTL, 0x1);
        ... (12 registers, all 0x1) ...
}
```

Upstream uses **`0x1`**, same order, same position (immediately before
`set_hwcg`). **Our tree matches upstream exactly.** KGSL's `0x0` reflects the
Android IOVA scheme, not a defect.

The in-tree comment claiming "Order and values match upstream/KGSL exactly" is
half-true — it matches upstream, not KGSL. Worth correcting so nobody "fixes" it
toward the Android value later. **Not a defect.**

### 2b. UCHE trap / write-through / write-range — **match upstream exactly**

Upstream (non-a7xx):
```c
gpu_write64(gpu, REG_A6XX_UCHE_WRITE_RANGE_MAX, uche_trap_base + 0xfc0);
gpu_write64(gpu, REG_A6XX_UCHE_TRAP_BASE,      uche_trap_base);
gpu_write64(gpu, REG_A6XX_UCHE_WRITE_THRU_BASE, uche_trap_base);
... uche_trap_base = 0x1fffffffff000ull;   (a6xx_gpu.c:2777, unconditional)
```

Ours:
```
UCHE_WRITE_RANGE_MAX_LO 0xffffffc0 / HI 0x0001ffff  -> 0x1fffffffffffffc0
UCHE_TRAP_BASE_LO       0xfffff000 / HI 0x0001ffff  -> 0x1fffffffff000
UCHE_WRITE_THRU_BASE_LO 0xfffff000 / HI 0x0001ffff  -> 0x1fffffffff000
```

`0x0001ffff_fffff000 == 0x1fffffffff000` (same 48-bit value), and
`WRITE_RANGE_MAX = base + 0xfc0` holds. **Correct, not a defect.**

---

## Part 3 — systematic upstream-vs-ours init screen

Method: extract every `gpu_write(gpu, REG_A6XX_*, …)` and `WRITE(REG_A6XX_*, …)`
from upstream `a6xx_gpu.c` and from ours, compare register-by-register.
**This is a screen, not a diagnosis.** Differences must be chip-gated before they
mean anything.

### Registers upstream writes that our tree never references
(verified by direct grep across `a6xx_gpu.c`, `a6xx_gmu.c`, `a6xx_fault.c` —
zero occurrences)

| register | upstream value | chip gate | relevant to a640? |
|---|---|---|---|
| `UCHE_CMDQ_CONFIG` | `0x90` / `0x66906` | `adreno_is_a690` / a660,a663 / a7xx | **NO** |
| `CP_CHICKEN_DBG` | `0x00028801` / `BIT(24)` | a7xx / `adreno_is_a702` | **NO** |
| `TPL1_DBG_ECO_CNTL1` | `0xc0700` | — | check |
| `CP_MEM_POOL_DBG_ADDR` | `47` | appears ungated; KGSL also writes 47 | low — debug address |
| `RBBM_GPR0_CNTL` | `0` | — | low |
| `GBIF_SCACHE_CNTL0` | `FIELD_PREP(GENMASK(14,10))` | — | **worth checking** — we program only `_CNTL1` |

### Same register, different literal value — assessed
| register | upstream | ours | assessment |
|---|---|---|---|
| `UCHE_GBIF_GX_CONFIG` | `0x10240e0` (a7xx only) | `0x10200F9` (KGSL) | now tested; no change |
| `CP_PROTECT_CNTL` | `ACCESS_PROT_EN｜FAULT_ON_VIOL_EN｜LAST_SPAN_INF_RANGE` | `0x00000003` | **ours enables protection without fault-on-violation** — see below |
| `CP_SQE_STAT_ADDR` | `1` | `0` | stats-counter destination; not a shader store |
| `PC_DBG_ECO_CNTL` | `info->prim_fifo_threshold` | `(0x300 << 11)` | **shader pipe FIFO threshold — value divergence** |
| `CP_MEM_POOL_SIZE` | `48` | `128` | low |
| `RBBM_CLOCK_CNTL` | `state ? clock_cntl_on : 0` | same shape | a6xx value already verified `0x8aa8aa82` |
| `RB_NC_MODE_CNTL` / `SP_NC_MODE_CNTL` / `TPL1_NC_MODE_CNTL` / `UCHE_MODE_CNTL` | composed from UBWC config fields | simplified constants | plausibly equal when `ubwc_mode=0` (our case) |
| `UCHE_CACHE_WAYS` | `BIT(23)` (a7xx) | `0x4` | ours matches upstream's **a6xx** branch |
| `UCHE_CLIENT_PF` | `BIT(7)` | `1` | a7xx branch vs ours |
| `VBIF_XIN_HALT_CTRL0` | mask | `0xf` | VBIF — not used on GBIF parts |
| `GBIF_SCACHE_CNTL1` | `FIELD_PREP(GENMASK(29,25))` | `scid_val` | needs field comparison |

### Two items worth a second look
1. **`CP_PROTECT_CNTL`**: upstream sets
   `ACCESS_PROT_EN | FAULT_ON_VIOL_EN | LAST_SPAN_INF_RANGE`. Ours writes `0x3`
   = `ACCESS_PROT_EN | FAULT_ON_VIOL_EN`, omitting `LAST_SPAN_INF_RANGE`. The
   register-protection *tables* match upstream (verified earlier). The missing
   span bit changes which register ranges are considered "in span".
2. **`GBIF_SCACHE_CNTL0`** is programmed by upstream and KGSL; we program only
   `_CNTL1`. SCID/LLC routing was tested in image 242 and refuted, but the
   `_CNTL0` half was never independently exercised.

---

## Where this leaves the investigation

**Eliminated, each with evidence rather than absence of evidence:**

| candidate | how it was closed |
|---|---|
| A — UCHE/cache flush | present, timestamped, ordered after draws (capture evidence) |
| B — `UCHE_GBIF_GX_CONFIG` | written per KGSL, **verified executed**, no behavioural change |
| C — SMMU write permissions | GEM buffers mapped `IOMMU_READ｜IOMMU_WRITE` |
| D — secure gate | no `CP_REGISTER_PROTECTION_ERROR` ever raised |
| SQE ucode staleness | newer upstream ucode, identical behaviour |
| Mesa render state | field-correct vs real XML; registers live and workload-tracking |
| `ADDR_MODE_CNTL` | matches upstream |
| UCHE trap/thru/range | matches upstream exactly |

The remaining screens (`CP_PROTECT_CNTL` span bit, `GBIF_SCACHE_CNTL0`,
`PC_DBG_ECO_CNTL` threshold) are all **init-time register values**, and every one
of them would need a build+flash to test — which is exactly the class of action
now gated behind approval.

**Honest assessment:** the cheap hypotheses are gone. What remains is a
systematic upstream-vs-CAF divergence hunt in hw_init, one register at a time,
each costing a build and a flash. That is a legitimate strategy but it is slow and
this project has a poor hit rate per flash (eleven instruments have produced
confident wrong answers).

The alternative worth weighing: the divergence between upstream msm and this CAF
tree is now the only remaining structural suspect, and it is testable **without
any hardware experiment** by building a mainline-msm kernel for this tree — the
`consult/PLAN.md` §X6 "known-good stack on this silicon" idea, which has not been
tried and would either reproduce the bug in a much smaller code surface or
immediately exonerate everything above it.

## Device state

On image 268, booted clean, GPU healthy. `UCHE_GBIF_GX_CONFIG` write is gated on
`a6xx.uche_gbif_rw` which is now `1` on this image — to return to the neutral
baseline, reflash 263 (or any image without the flag). The change is inert, so
leaving it on does not affect any future experiment.