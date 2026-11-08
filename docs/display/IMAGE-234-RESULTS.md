# Image 234 — the hang is fixed

## Headline

The dma_fence hang is **solved**. Image 233's `pixelreadback.py` blocked
forever in `dma_fence_default_wait`. On 234 it runs to completion, exits 0,
and the GPU raises **no faults at all**.

A new, separate problem is now exposed: the render target is still all zeros.

## The fix

`msm_gpu_aspace_new()` (msm_gpu_mmu.c) now rebases the GPU aperture off zero,
using CAF's documented 64-bit base `KGSL_IOMMU_SVM_BASE64 = 0x700000000ULL`
(kgsl_iommu.h:39). d2q is the 64-bit path because `sm8150-gpu.dtsi` does not
set the DT feature `qcom,force-32bit` (kgsl_iommu.c:3068).

## All four predicted checks passed

```
aspace 'gfx3d': rebasing aperture 0x0 -> 0x700000000 (end 0x7fffffffff)
aspace 'gfx3d' on cb 'gfx3d_user': iova 0x700000000-0x7fffffffff
RINGHW:    rb_iova=0x700001000
RINGHW[10] memptrs_iova=0x700000000 rbmemptr_rptr=0x700000000 rbmemptr_fence=0x700000004
```

| check | predicted | actual |
|---|---|---|
| rebase print | yes | yes |
| `memptrs_iova` | non-zero | `0x700000000` |
| `rbmemptr_fence` | `memptrs_iova + 4` | `0x700000004` ✓ |
| `rb_iova` | `0x701000` | `0x700001000` ✓ |

ACD sent (`ret=0`), `FORENSIC VERDICT GOOD BABEFACE`, `HWINIT[25] return ret=0`.

## 233 vs 234, same test

| | 233 | 234 |
|---|---|---|
| pixelreadback.py | hung in `dma_fence_default_wait` | `EXIT_0`, SUMMARY 0/6 |
| `hangcheck recover!` | 12 | **0** |
| `DIAG231` fault dumps | many | **0** |
| render target | (never reached) | `(0,0,0,0)` everywhere |

The fence now retires and the GPU stops faulting. The completion accounting
that was severed is intact.

## New problem: nothing renders

All six checks fail with `got=(0, 0, 0, 0)`:

- plain clear (green, magenta)
- scissored clear, both inside and outside the rect
- shader triangle, both corner and full coverage

Scissor geometry is honoured in neither direction, and the shader path (ROP,
not the clear fast path) fails identically. So this is not a blitter or
clear-path bug: the GPU is not writing the render target at all.

### RETRACTED: the domain is an identity mapper (was the prime suspect)

This was my leading hypothesis for the all-zero renders. **It is wrong.**

Claimed: `iommu_domain_alloc(&platform_bus_type)` yields dma-iopmu, so BO
mappings never reach real GPU page tables. Evidence I cited: the aperture
printed `0x0-0x7fffffffff`, and arm-smmu clamps to 4 GB-1
(`arm-smmu.c:2300-2303`).

Both legs fail on this tree:

1. **`drivers/iommu/dma-iommu.c` exists but never registers for platform bus.**
   More importantly, this CAF kernel does **not** use
   `bus->dma_ops->iommu_ops`. `__iommu_domain_alloc()` (drivers/iommu/iommu.c:
   1280-1300) uses `bus->iommu_ops` directly, and
   `arm-smmu-v3.c:2859` does:
   ```c
   if (platform_bus_type.iommu_ops != &arm_smmu_ops) {
           ret = bus_set_iommu(&platform_bus_type, &arm_smmu_ops);
   ```
   So the domain gets **real arm-smmu ops**, not an identity mapper.
2. **The 4 GB-1 clamp is in the wrong driver.** The active SMMU is
   `arm-smmu-v3.c`, not `arm-smmu.c`. `arm-smmu-v3.c:1620` sets:
   ```c
   domain->geometry.aperture_end = (1UL << ias) - 1;
   domain->geometry.force_aperture = true;
   ```
   with no clamp. At `ias = 39` that is `(1<<39)-1` = `0x7FFFFFFFFF` —
   **exactly the value we printed.** The aperture was arm-smmu-v3's own
   default all along.

Net effect on the 234 fix: the rebase was still **necessary and correct** —
arm-smmu-v3 never sets `aperture_start`, so it came up 0 and the first
allocation (memptrs) landed on IOVA 0. But my explanation of *why* was wrong.
The rendered output being zero is therefore **not** explained by missing
page tables; something else is responsible.

### Next image (235) — direction revised

The "resolve the iommus phandle to the SMMU master bus" plan is **withdrawn**;
the domain is already a real arm-smmu-v3 domain. Remaining candidates for
all-zero renders:

- render target register programming (CCU/RB ROP config) missing or wrong
- a cache/coherency failure on the readback path
- the draw never actually dispatching
- SDE/APRIV privilege not established for UCHE writes
- BO memory not contiguous or not DMA-able

Awaiting sub-agent cross-checks before choosing.

## Still-open loose end from 233

`recover_work` has one `queue_work` (msm_gpu.c:438, inside
`hangcheck_handler`'s LOCKUP branch), yet on 233 `recover_worker enter` printed
12 times while `LOCKUP` and `enter fence=` printed **0** times inside a
retained ring-buffer window. Unexplained; not revisited.

## Log-loss note for 234's first run

The device froze shortly after a trigger at 662s. `/proc/last_kmsg` had been
overwritten by the intervening recovery boot and `pstore` was empty (a hang
does not write pstore, only a panic does), so that run produced no data. The
result above is from a second boot of the same kernel `#41`.

The on-device `while true; do dmesg > f; done` logger also died after one
iteration — it is not reliable on this device.