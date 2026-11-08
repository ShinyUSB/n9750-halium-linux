# Image 155 run — GPU_SET exonerated; flash discipline fixed

## Run (verified 155 live: SKIPSET155 printed)
- Full ACKs + vote ACK + GBIF + cp_init → **selftest 10/10** →
  `[25] ret=0`, all WITHOUT GPU_SET. Then `NON_SECURE_WDT` reset.
- GPU_SET is definitively unnecessary for power AND its absence does
  not prevent death. Removed from suspect list (both directions).

## Flash anomaly resolved
- Prior "155 didn't stick": single transient flash failure (upload OK,
  old image booted). No slots on this device. Fix adopted: verify live
  cmdline flags BEFORE every trigger run from now on.

## Standing suspect list (present in all deaths, absent in all survivals)
- hw_init register programming + CP execution + OOB clears. GPU_SET now
  excluded. Next isolation: 150-hold (skip OOB clears) needs a success
  run (its only run was silent/suppress).

Image 155 `0d1fe069…`. SDE 0, DT = 121, prod untouched. Stopping.
