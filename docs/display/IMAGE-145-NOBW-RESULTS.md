# Image 145 run — ECP-without-BW = YES; fault #2 (CB1/0x5) decoded further

## ECP question: ANSWERED YES
- `BW145: SKIPPED` → `ECP enter` → **`HFI-ACK id=11`** → core(14) →
  start(10) all error=0. With real ARC votes + PERF ACK, ECP does NOT
  need BW state (113's failure was its zero-votes, now proven).
- So the BW table is not a chain prerequisite; its side-effects theory
  loses its main pillar (fault occurred WITHOUT any BW send).

## Fault #2 (550µs after START ACK, during ECP wait)
- `FAULT smmu=2ca0000.kgsl-smmu iova=0x70290930 cb=1 sid=0x5
  fsr=0x40000402(TF)`. Panic (same `panic-on-unhandled` path).
- New facts: instance IS the GPU SMMU; CB1 (unowned by us — vendor has
  no CB1; its ttbr points at live foreign tables); IOVA real-looking
  (not fill); no storm/NACKs before it.
- vs fault #1 (`cb=1 sid=0x405 iova=0x55555561` fill-garbage): same CB,
  same stream family (mask 0x400 covers both), different IOVAs. Pattern:
  CB1-routed 0x40x-family traffic faulting on unmapped IOVAs.

## Leading theory (to verify, not assumed)
- Bootloader/TZ leaves SMRs routing some 0x40x traffic → CB1; our
  driver's `find_sme` may reuse rather than override; legit-or-rogue
  GMU traffic on 0x5/0x405 then faults in stale tables instead of CB5.
- Normal traffic (0x400) routes correctly (other SMR) — hence HFI works.

## Next (146)
- Log every SME alloc (sid/mask/idx→cb) at attach in arm-smmu.c: makes
  the real routing table visible, zero behavioral change.
- Then decide: force-fresh SMRs for our CBs vs map-vs-tolerate.

Image 145 `f2f6040f…`. SDE 0, DT = 121, prod untouched. Stopping.
