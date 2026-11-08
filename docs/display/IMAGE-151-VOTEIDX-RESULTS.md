# Image 151 run — success profile unchanged; vote-idx neutral

## Run
- `VOTE151: gx=0x9 mx=0x6` (writes only) → full ACK chain (1/4/3/11/14/10
  + vote 30) → sticky writes → `cp_init 0` → **selftest 10/10** →
  `[25] ret=0` → later `NON_SECURE_WDT` reset.
- No timeouts anywhere → `HFI-T` forensics untested live (code armed).
- No storm/faults/panics.

## Reading
- Vote-idx writes neither helped nor hurt vs 133/136 (identical shape).
  Effect unisolated (DCVS writes explain stickiness alone). Keep them:
  vendor-faithful, zero-cost, zero-risk (plain MMIO, verified landing).
- The WDT-after-success pattern persists independent of all HFI-side
  changes — reinforces that post-init survival is a separate domain
  (power/userspace), not HFI sequencing.

Image 151 `c42e2e1d…`. SDE 0, DT = 121, prod untouched. Stopping.
