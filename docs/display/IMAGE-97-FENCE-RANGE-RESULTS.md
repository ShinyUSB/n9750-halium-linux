# Image 97/98 — FENCE_RANGE ruled out; new host teardown Oops recorded

(Note: built as image 98 `f57caaf1…` — image 97 was the SYSRESET build.
Filename kept as instructed.)

## Fence write verified live

`FENCE_RANGE_0 write (value=0x80A008A0)` executed in `fw_start`.

## Verdict B — fence ruled out

`D-timeout: INITRES=0xbaddface`, `fw_start returned (ret=-110)`, gate held.
Identical failed-boot verdict as images 95–97. **No GMU SMMU translation
fault anywhere this boot** (zero context-fault lines).

## C — new fault (host side, recorded exactly, not fixed)

~7 ms after the timeout path, the trigger task Oopses **in host code**
(no GMU involvement):

```
Internal error: 96000010 [#1] PREEMPT SMP
pc : __arm_smmu_tlb_sync+0xa0/0x3a8   (x0 = 0, NULL deref)
Call trace:
  __arm_smmu_tlb_sync → arm_smmu_tlb_sync_context → arm_smmu_tlb_inv_context_s1
  → arm_lpae_unmap → arm_smmu_unmap → __iommu_unmap → iommu_unmap
  → a6xx_gmu_remove → a6xx_destroy → adreno_load_gpu
  → a6xx_gpu_load_store → sysfs write path
```

Mechanism: `adreno_load_gpu` ignores the `pm_runtime_get_sync` resume error,
`msm_gpu_hw_init` fails via our diagnostic gate, so it calls `destroy()` →
`a6xx_gmu_remove` unmaps live buffers → TLB sync dereferences NULL (SMMU
already suspended/torn down or domain state inconsistent at that point).

Why it fired on this run but not on 96/97 is undetermined (same -110 path;
candidates: GMU-state-dependent stop/teardown interleave, timing). It is a
host teardown bug in the failure path, unrelated to the fence value and not
a GMU DMA fault.

## Standing

- FENCE_RANGE (0x80A008A0): no effect on boot outcome. Ruled out.
- SMMU translation faults: absent for 4 consecutive images (95–98) whenever
  the poison page is mapped; the remaining issue is boot completion
  (`BADDFACE`) plus now this teardown path.
- SDE 0, DT +0, production untouched. No fix applied; stopping.