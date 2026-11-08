# Log-buffer / CP_MSG audit (read-only, no source changes, no flash)

## TASK 1 — three-way fw_start() table (firmware load → CM3_SYSRESET)

| register | ours | upstream v5.8 | vendor KGSL | purpose / notes |
|---|---|---|---|---|
| GENERAL_7 = 1 | yes | yes | yes | TCM retention. Match. |
| rpmh_ucode (PDC SEQ/TCS) | fixed region-relative (img 80) | seqptr/pdcptr split | cfg/seq split | Match (post-fix). |
| CM3_FW_INIT_RESULT = 0 | yes | yes | yes | Match. |
| CM3_BOOT_CONFIG | 0x02 | 0x02 | TCM_BOOT = 2 | Match. |
| HFI_QTBL_ADDR | hfi->iova (= 0x60000000) | hfi.iova (dyn ≥ 0x60001000) | hfi gmuaddr | Same mechanism; see dummy collision below. |
| HFI_QTBL_INFO = 1 | yes | yes | yes | Match. |
| AHB_FENCE_RANGE_0 | (1<<31)\|(0xa<<18)\|0xa0 | identical | GMU_FENCE_RANGE_MASK = (1<<31)\|((0xA<<2)<<18)\|(0x8A0) — DIFFERENT | v5.8 agrees with ours; KGSL disagrees with both. Flagged, not concluded. |
| CM3_CFG = 0x4052 | yes (img 86) | **ABSENT** | yes (+ NMI comment) | Samsung/KGSL-only workaround. v5.8 works upstream without it → not a universal boot requirement; consistent with img-86 negative result. |
| HFI_SFR_ADDR = chipid | yes | yes | yes | Match. |
| **CP_MSG = log iova\|size** | **MISSING** | yes (`log.iova \| (size/4K-1)`) | yes (`gmuaddr & ... \| (4K/4K-1)`) | Only delta in this range besides power_config caches (below). |
| power_config() | base set | + ICACHE_CONFIG=1, DCACHE_CONFIG=1 | (not compared) | v5.8 enables GMU caches; ours never does. Recorded as follow-up, not concluded. |
| → gmu_start → gfx_rail → sptprac → hfi | same shape | same shape | same shape | Match. |

CP_MSG constant is absent from our `a6xx_gmu.xml.h` (would need adding for any
future patch).

## TASK 2 — gmu->log audit

| | upstream v5.8 | vendor KGSL | ours |
|---|---|---|---|
| field | `struct a6xx_gmu_bo log;` (embedded) | `gmu->gmu_log` (memdesc ptr) | **none** |
| allocated | probe, `SZ_4K` | probe, `LOGMEM_SIZE = SZ_4K` | — |
| IOVA | dynamic, GMU uncached range [0x60001000, 0x80000000) | `gmuaddr` (same range class) | — |
| mapped/freed | yes / teardown | yes / remove | — |
| size | 4 KiB (matches prompt's reference) | 4 KiB | — |

Related v5.8-vs-ours map divergence found during this audit:

| object | v5.8 | ours |
|---|---|---|
| dummy 4K | fixed **0x60000000** | **never allocated — our HFI sits at 0x60000000** |
| hfi 16K | dynamic ≥ 0x60001000 | 0x60000000 (dummy's slot) |
| log 4K | dynamic ≥ 0x60001000 | missing |

## TASK 3 — fault addresses vs map

GMU map: hfi `0x60000000/0x4000`, icache `0x4000/0x3c000`, dcache `0x44000/0x3c000`.

| fault IOVA | location | log/HFI/cache-related? |
|---|---|---|
| 0x433c0 | icache–dcache gap | no (gap unmapped in v5.8 too) |
| 0xb54e0 | past dcache end (0x80000) | no |
| 0x81ce0 | 0x1ce0 past dcache end | no (adjacent, not inside) |
| 0x89ce0 | 0x9ce0 past dcache end | no |

None falls in a log-like region (0x6xxxxxxx), in HFI, or inside either cache.
A missing log buffer cannot produce reads here: with CP_MSG never written, the
register holds its reset value, and any log access derived from it would fault
near the reset address (≈0x0), not at these four scattered low addresses.

## TASK 4 — verdict

CP_MSG/log is a **diagnostic feature, not a boot requirement**: upstream
introduced it as "enable GMU log" (debugfs log readout); its absence cannot
plausibly explain GMU READ faults at addresses unrelated to any log region.
Do NOT write CP_MSG with NULL log (agreed — no such write exists), and do not
add the log buffer as a fix for these faults.

Stronger remaining leads from this audit (recorded, not acted on):
1. `ICACHE_CONFIG`/`DCACHE_CONFIG` never set (v5.8 sets both in power_config).
2. KGSL's `FENCE_RANGE_MASK` differs from the v5.8/ours value.
3. Our HFI occupies v5.8's dummy slot at 0x60000000 (no observed fault there,
   but the layout diverges).

## Prepared minimal patch (DESCRIBED, not applied — zero source changes made)

If log support is ever wanted for diagnostics: add `struct a6xx_gmu_bo log;`
to `struct a6xx_gmu`, allocate `SZ_4K` in probe after HFI (dynamic IOVA),
`REG_A6XX_GPU_GMU_CX_GMU_PWR_COL_CP_MSG` constant to `a6xx_gmu.xml.h`,
write `gmu_write(gmu, CP_MSG, log.iova | (log.size/SZ_4K - 1))` after SFR_ADDR,
free on teardown. Expected to change nothing about the current faults.

Build: nothing to compile (no modifications). Nothing flashed.
SDE/DT/IOMMU/PDC/parser/HFI/fences/submit/Mesa/Weston untouched.
Production tree untouched.