# IMAGE-136-SECVID-SKIP.md

## Classification
**D) HW_INIT_COMPLETES_PAST_SECVID** (B subsumed: skip allowed continue).

## Attempt log (PART 4)
- Attempt 1: vote ACK (`id=30 error=0`) → GBIF → `cp_init ret=0` →
  SECVID134 reached. No retries needed.

## Mode-1 run (PARTS 5–7)
- Pre-skip: `TRUST_CNTL=0x1 TSB=0/0/0 STATUS=0x1 RPTR=0x9 WPTR=0x9
  OOB_H2G=0x10000 OOB_G2H=0x0 RPMHPWR=0x0 mode=1`.
- `mode=1 SKIP direct SECVID_TRUST_CNTL write` printed (param fix verified).
- Continuation (existing markers): `[15]` → `[23]` OOB clear →
  `[24]` → **`[25] return ret=0`**. No new instrumentation was needed;
  no post-skip register snapshots exist beyond these markers
  (RBBM/CP state at C/D/E: not printed — existing markers carry flow only).
- OOB note: GPU_SET never ACKed (`H2G=0x10000`, `G2H=0x0`) yet all
  downstream worked — its ACK is definitively unnecessary.

## Outcome vs 135-mode-0
- Mode 0: synchronous stall AT the write (no `after` line ever).
- Mode 1: sails through to `ret=0`. The direct write is THE blocker;
  reads of the same register are fine (TZ write-trap profile).

## Post-run
- That boot later rebooted silently (exact cause unrecorded; no panic/
  Oops/SMMU/storm evidence in retained buffers).
- **New frontier: post-`hw_init` return** — DRM completion, GPU idleness,
  first real submission, or the silent reboot itself.
- Mode 2 stays STOPPED. No fixes applied in 136 beyond the param move.

Image 136 `209d4063819bdf68a93e32945bd193bc8bd609860f847d89ee4440e0b3c0ca97`.
SDE 0, DT = 121, production untouched.
