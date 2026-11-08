# IMAGE-137 TEST B (5 attempts) — NONLEGACY_HW_INIT_FAILS + vote/GX dissociation

## Classification: NONLEGACY_HW_INIT_FAILS
Nonlegacy path executes (vote sent when hfi allows; DCVS skipped every
run), but hw_init still fails at CPIDLE whenever GX is off.

## Attempt log
1. hfi silent 5s → -110 (vote unsent).
2. hfi silent 5s → -110 (vote unsent).
3. Full chain → vote TIMEOUT `-110` (E-class; POST frozen).
4. BADDFACE pre-HFI (`-5`).
5. Full chain → **vote ACK (id=30, ret=0)** → skip → hw_init walk →
   GBIF sticks → **RB/SQE writes ALL VOID** (`after=0x0`) →
   CPIDLE-FAIL → `[25] ret=-22` → POSTINIT `[40] ret=-22`.

## Decisive dissociation (att5 vs 133/136)
Vote ACK does NOT imply GX power: att5 ACKed yet every GX write
vaporized and STATUS stayed 0x0, while 133/136 ACK runs had sticky
writes and CP idle success. ACK is not sufficient; something else
(undetermined, possibly GMU-autonomous rail timing vs our immediate
register programming) decides. GBIF_QSB sticks in ALL states —
different power domain, confirmed 3rd time.

## Invariants
- No storm in att5 (zero `error=111`); no faults/panics any attempt.
- PRE snapshots identical all runs (`0xc0`/zeros).
- LAST_GOOD (att5): vote ACK + GBIF readbacks.
- FIRST_BAD (att5): `RINGHW-W after=0x0` → CPIDLE-FAIL → -22.

## TEST C: NOT RUN
Selftest re-proves `cp_init`, which fails -22 whenever GX is off;
nothing to extend until a passing baseline exists. Judged pointless,
not unsafe. Standing by for direction (e.g., GX-rail timing image).
