# Image 149 (build-only) — non-fatal GPU SMMU domains

## The one change (`msm_gpu_mmu.c`, CB setup)
`iommu_domain_set_attr(NON_FATAL_FAULTS=1)` on both GPU domains right
after alloc (logs if unsupported). Fault path then RESUME_TERMINATEs
instead of panicking: garbage-DMA becomes dev_err + waiter timeout,
which bounded retry recovers. Real mapping bugs still log with full
decode (143 hook intact).

## Checks
- full: EXIT 0, 0 errors. SDE/display 0; prod untouched.
- Packaged canonical path (probe/secure/nonlegacy/selftest, no skip).
- Image 149 `149-halium-boot-a6xx-nonfatal.img`
  sha256 `8db8d80b06e88440c4e8706d71c86a0e1acb698d05f16810fa86ea598b36006b`.

No flash performed; stopping.
