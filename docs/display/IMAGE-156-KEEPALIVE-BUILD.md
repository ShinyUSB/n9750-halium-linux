# Image 156 (build-only) — HFI keepalive votes (survival test)

## Theory under test
Post-success deaths (~1-12 min, silent, TZ-class) with driver fully
quiescent point at firmware-autonomous causes. If the firmware needs
ongoing host contact (votes/requests) after boot, going silent kills it.
Re-voting the proven GX vote every 10s tests exactly that.

## Diff
- `keepalive156` worker: re-sends `ack=1/freq=top/bw=0` via the existing
  sender, logs ret, reschedules (10s). Armed once after a successful
  probe vote, iff `a6xx.keepalive156=1`.
- Untouched: everything proven. Default off = identical behavior.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- Packaged all-flags + keepalive=1.
- Image 156 `156-halium-boot-a6xx-keepalive.img`
  sha256 `5face978c875a5f8f90180a3dba06e12cb076045c96b3218a52620edc755370c`.

## Readout
- Survival >>12 min with periodic `KEEPALIVE156: re-vote ret=0` → contact
  theory CONFIRMED (path to devfreq design).
- Death on schedule → exonerated, move to slumber/firmware-WDT theories.

No flash performed; stopping.
