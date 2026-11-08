# Image 148 run — (a)/(b) decided: routing correct, firmware DMA is garbage

## Fault table at fault time (kgsl-smmu)
- idx2 `{0x0/0x401}→CB0`, idx3 `{0x5/0x400}→CB1`, both fresh, plus
  unrelated test-device entries. No handoff, no reuse, no mutation.
- `FAULT smmu=2ca0000.kgsl-smmu iova=0x55555561 cb=1 sid=0x405
  fsr=TF`: stream 0x405 correctly matches idx3 → CB1 (gmu_kernel
  domain) → translation-faults on fill-garbage IOVA.
- Verdict: (b) with correct routing. The SMMU did its job; the GMU
  firmware issued DMA to `0x55555561`. Same family as NACKs/silence:
  firmware flakiness, 3rd SMMU instance overall (2× fill-garbage class).

## Consequence
- Guard untested (no reset reached — panic preempts returns).
- Retry untested (same reason).
- Correct response is NOT routing changes: make OUR two GPU domains
  non-fatal (`DOMAIN_ATTR_NON_FATAL_FAULTS`) so a firmware garbage-DMA
  becomes dev_err + waiter-timeout instead of panic; the existing
  bounded retry then recovers the run. Real mapping bugs still log
  loudly. (Production can re-harden later.)

Image 148 `fa7b670b…`. SDE 0, DT = 121, prod untouched. Stopping.
