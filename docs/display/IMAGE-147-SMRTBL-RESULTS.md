# Image 147 run — retry feature found a real regulator bug (panic)

## What happened
- Attempt 0: hfi silent 5s (`-110`, no ACKs at all).
- `RETRY142: attempt 1/2` → `a6xx_gmu_reset()` →
  `regulator_force_disable(gmu->gx)` → `unbalanced disables for
  pm8150l_s2_level` → WARN + BRK (`BUG()`) → panic → reset.
- SMR146 lines present (routing data available on demand).

## Root cause (ours, not firmware)
- Our resume enables CX GDSC but NEVER enables `gmu->gx` ("vdd").
- `reset()` unconditionally `regulator_force_disable(gmu->gx)` on a
  zero-count regulator → unbalanced-disable BUG → panic.
- Vendor enables both GDSCs at start; our port only does CX. The guard
  (disable-only-if-enabled) is the minimal correct fix; full GX-enable
  at resume is a separate, firmware-ownership-sensitive decision.

## Standing
- Retry logic itself works (triggered correctly, reset path entered).
- Blocked on the guard fix (next image). No storm/faults this run.
- SMMU fault lottery: no occurrence (normal traffic clean).

Image 147 `f7bb76b5…`. SDE 0, DT = 121, prod untouched. Stopping.
