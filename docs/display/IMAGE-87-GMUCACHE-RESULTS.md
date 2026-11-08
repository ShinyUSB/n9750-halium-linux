# Image 87 — fault CHANGED: permission fault inside icache (was: translation faults outside all maps)

## Exact new fault (from last_kmsg, read directly)

```
PRE_PDCSEQ:                       38.585188
ICACHE_CONFIG/DCACHE_CONFIG:      38.586464-38.586493 (both writes succeed)
before GMU start:                 38.586503
+1.46 ms after GMU-start entry (+1.77 ms after PRE_PDCSEQ):
arm-smmu 2ca0000.kgsl-smmu: Unhandled context fault:
  iova=0x00004dc0, cb=1, fsr=0x40000408, fsynr0=0x60063, fsynr1=0x7
Kernel panic: No Device SMMU Fault - SID=0x405
```

Decoded against `arm-smmu-regs.h` in our own tree:
- `0x40000408` = `FSR_SS | FSR_PF` — **permission fault** (was `FSR_SS | FSR_TF`,
  translation fault, on all previous boots).
- `FSYNR0[4]` (WnR) clear → **read** (previous faults had it set → writes).
- `0x4dc0` lies **inside** icache `[0x4000, 0x40000)` — first fault ever inside
  a mapped region.

## What changed and what it means (observed, not timed)

Enabling the caches moved the GMU from *writing to unmapped addresses* to
*reading its icache and being denied by the page-table permission bits*.
Our `a6xx_gmu_memory_alloc_fixed()` maps with `IOMMU_READ | IOMMU_WRITE` only;
v5.8 maps icache/dcache with `MSM_BO_MAP_PRIV` (`IOMMU_PRIV` → `AP_PRIV_RW`,
privileged-only PTEs). Our PTEs are user-accessible; the GMU's privileged
cache access faults on them. This is a code-level delta on the exact fault
path — not a timing inference.

## Unchanged as instructed

- HFI still `0x60000000`, no dummy page, no log/CP_MSG, no FENCE/GX/ACD/DDR
  changes; CM3_CFG/DDR/CX/PDC/IOMMU/parser/HFI untouched.
- SDE 0, DT +0, production untouched. Image 87
  `5af6c85aea220c07280b5e031de9e54e372b44c8019225f42ecc89482fc68207`.

## Proposed next single change (NOT applied)

Add `IOMMU_PRIV` to the `iommu_map()` prot flags in
`a6xx_gmu_memory_alloc_fixed()` — the helper is used only for icache/dcache,
so the change cannot leak onto hfi/debug. Exact v5.8 match, one flag.