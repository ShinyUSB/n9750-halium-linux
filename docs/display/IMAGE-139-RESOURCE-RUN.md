# IMAGE-139 run — healthy image; NON_SECURE_WDT again post-success

## Run (single trigger, t=35s)
- ATT138 n=1. Full ACK chain (1/4/3/11/14/10) + vote ACK (`id=30`,
  `hfi_ret=0`). `[25] return ret=0`.
- No WDT139 lines (GMU watchdog/fault never fired — again).
- Reset reason: `TZBSP_ERR_FATAL_NON_SECURE_WDT` (3rd post-success
  instance). No panic/fault/storm.

## Standing
- Image behaves exactly like 136/138-success runs (expected: only
  forensic deltas vs 138v2).
- Classification F (from build record) unchanged: no OPP mechanism to
  run, no mode rates exist.
- WDT forensics Vl1: GMU-side watchdog exonerated again; resets come
  from outside the GMU IRQ domain (TZ/system WDT).

Image 139 `b7833fb7…`. SDE 0, DT = 121, production untouched. Stopping.
