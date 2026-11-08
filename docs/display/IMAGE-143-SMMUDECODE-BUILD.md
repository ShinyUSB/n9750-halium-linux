# Image 143 (build-only) — SMMU fault decode extension

## Why
142's panic gave `iova=0x55555561 cb=1 sid=0x405` (fill-pattern IOVA in
an unowned CB) but not the faulting SMMU instance. SMR analysis shows
our CB masks intentionally overlap (0x400/0x401→CB0, 0x005/0x405→CB5),
so the instance + CB + fault-type decode decides everything.

## Diff (diagnostic only)
- `arm-smmu.c` hook signature + call site: pass `dev_name(smmu->dev)`.
- `a6xx_gmu.c` override: print `smmu=` first in the FAULT line.
- No transport/power/sequence changes. All 142 features retained.

## Checks
- full: EXIT 0, 0 errors; fast: 0/0. SDE 0, DT untouched, prod untouched.
- Packaged all-flags, a6xx-first cmdline.
- Image 143 `143-halium-boot-a6xx-smmudecode.img`
  sha256 `26969eb536fe50597a89d5d194486d01815d68ba90082d33b3fc5ee1aa5ee8d7`.

No flash performed; stopping.
