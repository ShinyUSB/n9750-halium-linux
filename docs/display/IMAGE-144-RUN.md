# Image 144 run — 10/10 again; fast death weakens battery theory

## Run (58% / 3.95V)
- Full ACKs + vote ACK + `[25] ret=0` + **SELFTEST 10/10** (`0x12→0x63`).
- Died <5s post-init (before first +5s vitals poll fired). No vitals data.
- No storm/faults/panics on record. Reset reason not captured this run
  (no TZ line retained for it).

## Reading
- 58% death weakens (not kills) the supply hypothesis: brownout needs a
  spike + weak source, still possible at any SoC, but headroom was ample.
- New prime suspect: the dummy BW table's TCS writes. Our canonical
  table programs REAL TCS addresses (0x500xx) with placeholder data
  (0x40000000…); if firmware acts on them asynchronously post-ACK, a
  system-level (NoC) fault fits the silent profile exactly.
- Testable next (single variable): PERF-sent + BW-skipped (cell never
  run: 113 skipped both, 115 skipped PERF). If deaths stop → BW
  implicated; if not → exonerated, move to slumber/devfreq/hangcheck.

Image 144 `97b1fce0…`. SDE 0, DT = 121, prod untouched. Stopping.
