# Image 170 — proper slumber handshake + vote cleanup + fast recovery

## Why (upstream audit, not guessing)
Current `torvalds/master` read end to end against our tree. Converged
already (no work): exact a640 BW table, TEST message, PDC/RSC uCode,
cold-boot-only, ACD/IFPC correctly skipped. Three real deltas left,
all about the firmware relationship:
1. Nonlegacy slumber goes over HFI (`PREPARE_SLUMBER`, id 33), never
   over the legacy BOOT_SLUMBER OOB (upstream's OOB table has no
   nonlegacy entries for it). Our notify_slumber does OOB-only — may
   be talking past the firmware.
2. Upstream drops all votes on shutdown; we leave the GX HFI vote
   standing (the known stale-vote system-breaker class).
3. Recovery resume grinds the 3-attempt lottery (15s+) while the HW
   fuse (bite ~7-11s after last pet) runs out mid-recovery — observed
   once. Single attempt: cooperative firmware answers attempt 0
   anyway; silent firmware dies faster either way.

## Diff (on top of 169)
- `a6xx_hfi.h/.c`: id 33 define + `a6xx_hfi_send_prep_slumber`
  (zero bw/freq per upstream TODO) via the generic sender.
- `notify_slumber`: PREP_SLUMBER first (nonlegacy), then the legacy
  OOB dance regardless; 0x0f check judges.
- `a6xx_gmu_stop`: GX vote to index 0 after slumber (HFI alive there).
- Recovery: `recover166_fast` collapses resume to 1 attempt.
- Untouched: everything else.

## Checks
- fast: 0 errors, 0 warnings (after one extern forward-decl).
- full: EXIT 0, 0 errors.
- Packaged with 148 cmdline (slumber165 defaults ON; the =0 render
  cmdline would silence everything new).
- Image 170 `170-halium-boot-a6xx-preslumber.img`
  sha256 `24322231ea986d3978604e1efb7fe301189e4ba5983010753f1cb64187ffc949`.

## Readout
- `PREPSLUM170: sent ret=0` + RPMHPWR=0x0f → correct handshake;
  survival is the verdict on the lifecycle theory.
- PREP NACK/timeout → firmware rejects it; OOB path stands alone.
- Fast recovery completes (`end ret=0`) → transparent survival;
  bite cadence becomes observable.

At flash: add `PREPSLUM170|SLUM170` (already in filter via SLUM165?
no — add both). No flash performed (needs Download mode); stopping.
