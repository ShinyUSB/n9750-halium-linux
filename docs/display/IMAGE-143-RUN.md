# Image 143 run — SELFTEST 10/10 PASS; reset cause now ambiguous

## Result
- Full ACKs + vote ACK + `[25] ret=0`, then **SELFTEST142 all 10 PASS**
  (`0x12→0x63`, STATUS=0x1 throughout). First confidence milestone hit.
- No hangcheck, no submit, no storm, no SMMU fault, no panic.
- Reset <30s post-init (before first vitals poll fired).

## Reset-cause problem
- TZ says `NOC_ERROR`, but `pimem_esr=[0x0]` (empty syndrome) AND the
  same record says `upload_cause = User press reset keys for 7 sec`.
  A genuine NoC transaction fault normally captures ESR detail.
- Cannot distinguish TZ-NOC from forced reset on this evidence.
  Uptime-at-death is bimodal (~60–120s vs 400–1500s+).

## Process fixes queued
- Vitals: first poll at +5s (not +30s) so short boots yield data.
- All future reads: log boot_id + wall time to end timeline confusion.
- Question for operator: any power-button contact around reset times?

Image 143 `26969eb5…`. SDE 0, DT untouched, prod untouched. Stopping.
