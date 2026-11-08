# Image 233 — Results and Root Cause: fence memptrs_iova == 0

## Verdict

The GPU is **not** faulting. Image 233 finally produced the diagnostic that
231 and 232 lost, and it points at software bookkeeping, not hardware.

## What image 233 changed

| change | purpose |
|---|---|
| `a6xx.send_acd=1` | kept — confirmed fix, GX clock comes up |
| `DIAG231` fault dump | `RBBM_STATUS3` / `INT_0_STATUS` / `CP_INTERRUPT_STATUS` / `CP_HW_FAULT` |
| GBIF halt writes | **removed** (231 readback hung the GPU; 232 lost to the UFS BUG) |
| `qcom,disable-lpm` on `qcom,ufshc` | kills the pre-existing UFS `rpmh_write_batch` BUG |

## Image 232 freeze — not the GPU

`/proc/last_kmsg` shows the identical pre-existing crash from image 228 and
from 21 archived captures going back to image ~1006:

```
Workqueue: ufs_clk_gating_0 ufshcd_ungate_work
pc : rpmh_write_batch+0x46c/0x47c
```

GPU bring-up had completed (ACD ok, BABEFACE good, `HWINIT[03]` printed) before
the UFS BUG killed the system at 640 s. The 232 freeze was unrelated bad luck,
not evidence about the GBIF writes.

`qcom,disable-lpm` is read by `ufs_qcom_parse_lpm()` (ufs-qcom.c:2010) and
gates the whole `UFSHCD_CAP_*` set at ufs-qcom.c:1492, so
`ufshcd_ungate_work` is never queued. Cost: UFS clocks stay on.

## DIAG231 output

```
DIAG231 status3=0x00500000 int0=0x00000000 cp_int=0x00000000 cp_hw_fault=0x00000000
```

All clean. No SMMU stall (bit 24 clear), no RBBM interrupts, no CP fault.

## Ring pointer advances

```
rb 00ae/00ae -> 00bd/00bd -> 00cc/00cc -> 00db/00db -> 00ea/00ea -> 00f9/00f9
```

+15 dwords per hangcheck, perfectly regular. The CP is executing the stream
steadily. The "hang" is not a CP hang.

## Root cause: `memptrs_iova == 0`

Runtime, this boot:

```
RINGHW[10] memptrs_iova=0x0 rbmemptr_rptr=0x0 rbmemptr_fence=0x4
RINGHW:    rb_iova=0x1000
```

Two allocations: the first got iova 0, the second 0x1000.

Chain:

1. `msm_gpu_aspace_new("gfx3d_user", "gfx3d")` (a6xx_gpu.c:2434) builds the
   GPU address space from a **context-bank** IOMMU domain.
2. `msm_gem_address_space_create()` (msm_gem_vma.c:277) calls
   `drm_mm_init(&aspace->mm, domain->geometry.aperture_start >> PAGE_SHIFT, ...)`.
3. **Nothing in the tree ever sets the aperture on the context-bank path.**
   `msm_gpu_create_address_space()` (msm_gpu.c:817-818) does set it, but our a6xx
   does not use that function. msm_gpu_mmu.c only *prints* the aperture
   (:179-180, :261-262), never assigns it.
4. Runtime confirms: `aperture=0x0-0x7fffffffff`.
5. So `drm_mm_insert_node` hands out `node.start == 0` to the first allocation
   (msm_gem_vma.c:233: `vma->iova = vma->node.start << PAGE_SHIFT`).
6. memptrs is the first allocation, so `adreno_gpu->memptrs_iova == 0`.
7. `rbmemptr(ring, fence)` = `0 + offsetof(fence)` = `0x4`.

The `CACHE_FLUSH_TS` event in `a6xx_cache_flush_ts()` (a6xx_gpu.c:704-708,
byte-identical to donor v419:104-108) is therefore programmed to write the
completed fence seqno to GPU address 0x4.

## Why the fence never retires

`memptrs->fence` is written by the CPU in exactly one place,
`adreno_gpu.c:119`, at **ringbuffer reset** (bring-up), seeded from
`completed_fence`. It is never advanced on the CPU path — upstream v419 relies
on the GPU echoing the value back through CACHE_FLUSH_TS, same as we do.

With the fence write landing at IOVA 0x4 rather than the mapped memptrs
object, `memptrs->fence` stays frozen at its bring-up value.

Consequence:

- `a6xx_last_fence()` returns that frozen value.
- The completed-fence dma_fence is never signalled, so userspace blocks in
  `dma_fence_default_wait` (observed: pixelreadback.py stuck in that wait).
- The GPU raises its own `RBBM_HANG_DETECT`, `msm_gpu_recover()` runs
  `recover_worker`, which replays the submit list -> repeat, forever.

Note `retire_worker` is queued on every hangcheck tick as an explicit
"workaround for missing irq" (msm_gpu.c:445-446), so the retire path itself
runs; it is the *value* that never moves.

### Unreconciled: recovery path

`recover_work` has exactly one `queue_work` in the tree (msm_gpu.c:438),
inside `hangcheck_handler()`'s LOCKUP branch. Yet on image 233:

- `HANGCHK141: recover_worker enter` appeared 12 times
- `HANGCHK141: enter fence=` appeared **0** times
- `HANGCHK141: LOCKUP` appeared **0** times

Ring buffer spans 34 s - 896 s, so these are not outside the retained window.
Both are `pr_info` in the same file. **This could not be explained from
source and is recorded as open.** It does not change the image 234
experiment, whose observable is simply whether the recovery spam stops.

## Deeper issue found while fixing: the domain may not be an SMMU

`0x7fffffffff` is the tell. arm-smmu clamps its aperture to 4 GB-1
(arm-smmu.c:2300-2303: `(SZ_1G * 4ULL) - 1`), so a real SMMU domain would have
reported `0xffffffff`. dma-iopmu's identity mapper reports
`0 .. 2^39-1` = `0x7fffffffff`, which is exactly what image 233 printed.

`msm_gpu_cb_get()` does `iommu_domain_alloc(&platform_bus_type)`
(msm_gpu_mmu.c:142). `iommu_domain_alloc()` only sets
`domain->ops = bus->dma_ops->iommu_ops`; for `platform_bus_type` that is
dma-iopmu. `msm_iommu_map()` then calls `iommu_map_sg()` against an identity
mapper, which is a no-op -- so BO "IOVAs" may never reach real GPU page
tables at all.

CAF does not hit this: `kgsl_mmu_get_bus()` (kgsl_mmu.h:427-429) resolves the
bus of the **SMMU master** from the DT `iommus` phandle
(`iommus = <&kgsl_smmu 0x0 0x401>`, sm8150-gpu.dtsi:334-339) rather than
hardcoding `platform_bus_type`. `msm_iommu_get_bus()` is referenced by
kgsl_mmu.h but is **not present** in the partial vendor extract on disk, so
its body was not verified.

**This may be the true root cause, and the aperture fix may only be a
partial fix.** Image 234 tests the aperture hypothesis cleanly; if it still
hangs, the domain is the thing to fix next.

## Claims retracted during this investigation

Recorded because they were wrong and cost time:

- "memptrs->fence has no writer at all" — the GPU is the intended writer;
  v419 also writes it only at init. The defect is the *address*, not the
  absence of a writer.
- "the GPU is told to write to a bogus address 0x4" — iova 0 page **is** the
  memptrs page; 0 + offsetof(fence) is self-consistent, just not the address
  the SMMU was configured for.
- "line 237 copies memptrs_iova after hw_init" — it is *before* (line 237 vs
  line 242), and `info->init()` allocates memptrs even earlier. Ordering is fine.
- "CP_SCRATCH_REG7 == seqno" — no source reference; stale instrumentation.
  The `fence %x` in the hangcheck log is `ring->seqno` (CPU), not
  `memptrs->fence`.

Also checked and found clean, so not the cause:

- `A6XX_INT_MASK` — identical bit-for-bit to donor master, includes
  `CP_CACHE_FLUSH_TS`. Written at hw_init (ours :1550).
- `a6xx_irq` body — matches donor apart from missing `irq_poll_fence` and
  `keepalive_vote`, neither of which gates the timer-driven retire path.
- `a6xx_cache_flush_ts()` — byte-identical to v419.
- `retire_worker` — queued every hangcheck tick, so it does run.

## Next step

Set a non-zero `aperture_start` on the context-bank domain so the first
allocation does not land on iova 0.

**Done in image 234**, in `msm_gpu_aspace_new()` (msm_gpu_mmu.c), which is the
a6xx path:

```c
#define MSM_GPU_IOVA_BASE_64	0x700000000ULL
```

Value provenance, not invented: CAF `kgsl_iommu.h:39`
`#define KGSL_IOMMU_SVM_BASE64 0x700000000ULL`. CAF picks 32 vs 64 bit from
DT feature `qcom,force-32bit` (kgsl_iommu.c:3068); `sm8150-gpu.dtsi` does not
set it, so d2q is the 64-bit path. The change is guarded — it only fires when
`aperture_start == 0`, and refuses to rebase if the window would become empty.

**Verification for image 234:**

1. `aspace 'gfx3d': rebasing aperture 0x0 -> 0x700000000` in dmesg
2. `RINGHW[10] memptrs_iova` non-zero, `rbmemptr_fence == memptrs_iova + 4`
3. `rb_iova` now `0x701000`, no longer `0x1000`
4. **The `hangcheck recover!` spam must stop**

If (4) happens the aperture was the bug. If it does not, the domain is
probably the real problem (see above) and image 235 should resolve the
`iommus` phandle to the SMMU master's bus the way CAF's `kgsl_mmu_get_bus()`
does, instead of `iommu_domain_alloc(&platform_bus_type)`.