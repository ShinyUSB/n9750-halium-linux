# Image 151 (build-only) — timeout forensics + vote-idx without OOB

## Diff
- `a6xx_hfi.c`: every HFI timeout now prints `HFI-T` (q0/q1 indices,
  HFI+GMU IRQ counts and deltas, INTR_INFO/MASK, H2G_RAW). IRQ bases
  snapshotted at send.
- `a6xx_gmu.c` fw_start non-legacy branch: GX/MX vote-idx writes +
  BOOT_SLUMBER_OPTION, OOB skipped (`VOTE151` log; guarded on
  `nr_gpu_freqs>0`).
- Untouched: all proven paths; single functional change (the votes).

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- SDE/display 0 (verify at flash); DT/prod untouched (2 adreno files).
- Packaged probe/secure/nonlegacy/selftest (canonical, no skip).
- Image 151 `151-halium-boot-a6xx-voteidx.img`
  sha256 `c42e2e1d6e714d0f6271ef176fb9229db7253cc676f795c2d0d3f0cbd9f412fa`.

## Interpretation of the consultant round (recorded, not built on blindly)
- Both consultants independently converged on firmware power-state as
  #1 (one via blocked-vote theory matching our DCVS finding, one via
  gated-thread ranking). Each missed the other's secondary point
  (host IRQ path; watchdog/NACK semantics) — combined coverage adopted.
- Web search unavailable (tool errors); repo + consultants carried it.

No flash performed; stopping.
