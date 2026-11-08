# Image 146 rerun (no-BW) — perfect run, then WDT reset

## Run (trigger t=46s)
- GOOD BABEFACE → version/PERF ACK → BW145 skipped → ECP/core/start ACK →
  vote ACK (`id=30`) → GBIF → cp_init → **SELFTEST 10/10** →
  `[25] ret=0`.
- Latest reset reason in buffer: `NON_SECURE_WDT` (NOC entry is older
  appended content). No storm/faults/panics on this run.

## Standing
- No-BW path: 2/2 full successes (146 runs). BW table definitively not
  required for bring-up OR survival-to-hw_init.
- Death-with-success again (WDT class). Device left idle on fresh boot
  for the longevity watch — DO NOT TRIGGER until the watch concludes.

Image 146 `55c5a24c…`. Stopping.
