# SMMU audit — software-only, no hardware state changed

Date: 2026-10-18
Scope: page-table permissions, IOVA mapping, buffer ownership, fault diagnostics.
Constraint: no fault injection, no live register access, no device changes.

**Kept deliberately separate from the GBIF experiment so observed facts are not
confused with inference.**

## Result: candidate C is essentially ELIMINATED

GPU buffers — including the render target and the compute SSBO — are mapped with
**read-write permission**. There is no code path that strips write access.

## Observed facts (source, not inference)

### 1. The protection bits
```
msm_gem_vma.c:264
    unsigned int prot = IOMMU_READ | IOMMU_WRITE;
msm_gem_vma.c:278
    prot |= IOMMU_USE_LLC_NWA;
msm_gem_vma.c:280
    ret = aspace->mmu->funcs->map(aspace->mmu, vma->iova, sgt, size, prot);
```

`IOMMU_USE_LLC_NWA` is added because the domain attribute is set (see §3); it is
an LLC cacheability hint, not a permission change.

### 2. The mapping call chain ends in a plain `iommu_map_sg`
```
msm_gem_vma.c:280   funcs->map(...)
  -> msm_iommu.c:93  static const struct msm_mmu_funcs funcs = { .map = msm_iommu_map, ... }
  -> msm_iommu.c:60-71
       ret = iommu_map_sg(iommu->domain, iova, sgt->sgl, sgt->nents, prot);
```
`prot` is passed through unchanged. Nothing in this chain can downgrade write.

### 3. Domain attributes
`msm_gpu_mmu.c`:
- `DOMAIN_ATTR_USE_LLC_NWA = 1`, set **before** `iommu_attach_device()`
  (image 240; the comment records that setting it afterwards was refused `-EBUSY`
  because `arm_smmu_domain_finalise_s1()` had already fixed the page-table format)
- `DOMAIN_ATTR_NON_FATAL_FAULTS = 1` (image 149)
- `iommu_domain_alloc(&platform_bus_type)` then `iommu_attach_device()`

### 4. There is no second MMU backend in play for GEM
Two `msm_mmu_funcs` exist in the tree:
- `msm_iommu.c:93` — has `.map`/`.unmap`; this is what `msm_gem_vma.c:280` calls
- `msm_smmu.c:325` — has `.map_dma_buf`/`.unmap_dma_buf`; this is the **display**
  path (`msm_smmu_client`, `client->secure`, `SDE_EVT32`, `ss_smmu_debug_map`)

The GEM path uses the first. `msm_smmu.c` is not involved in render-target or SSBO
mapping and is not a candidate here.

### 5. `IOMMU_NOEXEC` is deliberately absent
`msm_gem_vma.c` records that image 236 added `IOMMU_NOEXEC` and broke bring-up
(`hw_init ret=-22`, SQE ucode became execute-never, CP could not fetch). CAF
exempts microcode per-BO via `KGSL_MEMDESC_UCODE`, which this tree cannot express,
so `NOEXEC` was left out entirely. It is hardening, not a functional requirement.
Noted because "the microcode buffer cannot be fetched" is a failure mode we have
already seen — and it is correctly *not* active.

### 6. Fault diagnostics present, and never triggered
- `a6xx_gpu.c:3002` decodes `A6XX_RBBM_STATUS3_SMMU_STALLED_ON_FAULT` and logs
  `*** SMMU_STALLED_ON_FAULT ***`
- `a6xx_gpu.c:1697` decodes `A6XX_CP_INT_CP_REGISTER_PROTECTION_ERROR`
- Observed in every capture: `CP_INTERRUPT_STATUS = 0`, `int0 = 0`, no stall bit

## Inference

Because the page tables grant write and no fault is raised, an SMMU-permission
mechanism cannot explain a *silently dropped* shader-domain store. The remaining
theoretical SMMU-shaped explanation would be a translation failure that is masked
by `DOMAIN_ATTR_NON_FATAL_FAULTS=1` — but that turns faults into `dev_err`, and
no such `dev_err` was seen in the surviving captures.

**Confidence: eliminated, high.** (Note this is stronger than the "weak" rating
in `ANALYSIS-FOUR-CANDIDATES-2026-10-18.md`, which was written before this pass.)

## What would falsify this

If the GBIF test on image 268 is negative, the cheapest way to re-open C would be
a boot-time dump of the actual page-table descriptor for one known IOVA, read via
the SMMU's own debug interface — not by MMIO, and not on a live GPU. That is a
different class of action from anything done here and is not proposed now.

## Residual risk

`DOMAIN_ATTR_NON_FATAL_FAULTS=1` was introduced because "GMU firmware sporadically
DMAs to garbage IOVAs (0x55555561-class, 3 samples)". That is itself evidence that
this IOMMU does produce faults on bad addresses — so the absence of faults in our
runs is meaningful evidence that the GPU is not issuing bad addresses, not evidence
that faults are being swallowed.

## Not audited in this pass

- Device-tree IOMMU stream-ID / master-SID configuration
- `msm_gem_prime.c` import path (`dma_buf_attach` at :118) — relevant only to
  imported buffers, not to Mesa-allocated RT/SSBO
- Cache coherency between CPU prefill and GPU store (already covered by the
  corpus's all-BOs-`MSM_BO_WC` finding and by the oracle-validated prefill test)