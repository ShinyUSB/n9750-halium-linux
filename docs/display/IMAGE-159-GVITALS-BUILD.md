# Image 159 (build-only) — GMU-only vitals poller

## Why
Deaths strike a quiescent driver minutes post-success with zero kernel
record. GMU-side state trend (SPTPRAC/RPMH/votes/OOB/IRQ counts) to the
moment of death is the missing evidence. GPU-side reads excluded
(GX-collapse NOC hazard per image 138 lesson); GMU CSR reads never
faulted in our history.

## Diff
- `gvitals157_*` delayed_work: GMU-only snapshot, first +5s then 30s,
  armed at every resume end in nonlegacy mode. Self-diagnosing: if a
  vitals read itself faults, the last-printed line proves it.
- Untouched: everything proven.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- Packaged canonical flags (probe/secure/nonlegacy/selftest, no skip,
  no hold, no keepalive).
- Image 159 `159-halium-boot-a6xx-gvitals.img`
  sha256 `6fae5c8f555326498001a7636c6e6bf92aeddfcea3c307c017ffddbc9c93f0ed`.

No flash performed; stopping.
