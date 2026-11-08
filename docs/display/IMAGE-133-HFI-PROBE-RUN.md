# IMAGE-133-HFI-PROBE-RUN.md

## Classification
**C) HFI_VOTE_ACKS_GX_TURNS_ON** — with one caveat (below): SPTPRAC
CLK_OFF bit stayed set while every functional datum shows GX alive.

## PART 1 (cmdline)
`a6xx.gmu_hfi_probe=1` verified in `/proc/cmdline`.

## PART 2 — probe record
- `GX132-PRE: SPTPRAC=0xc0 RPMHPWR=0x0 GXVOTE=0x0 MXVOTE=0x0
  GXGDSCR=0x222001 RB_*=0` (all zero).
- Request dwords: `ack_type=1 freq=6(idx) bw=0` (4 dwords).
- `HFI-ACK id=30 seq=6 error=0`; header `0x0070041e`, `size_dw=4`,
  `ret=0`. No block-beyond-ACK observed (returned immediately after).
- POST / +100us / +1ms: all fields IDENTICAL to PRE (no gradual ramp
  visible at these grains).
- Zero `error=111` anywhere this boot: no storm whatsoever.

## PART 3 — transition evidence (functional, not bit-based)
- Caveat: SPTPRAC CLK_OFF never cleared in any snapshot.
- But immediately after the vote: `RB_CNTL after=0x20c`,
  `RB_BASE after=0x1000`, `SQE after=0x9000` (first sticky writes ever);
  `RBBM_STATUS 0x0→0x1`; `CP_POST_FLUSH RPTR=WPTR=0x9 STATUS=0xc00015`;
  **`cp_init ret=0`** (first idle success ever).
- The vote is the only delta vs six prior `-22` runs on identical bytes.
  Correlation is exact (writes stick starting at the first post-vote
  register access); mechanism (which rail/clock the firmware raised)
  undetermined — SPTPRAC bit is evidently not authoritative.

## PART 4 — boot survival / new frontier
- hw_init walked `[12]-cp_init ret=0`, then hung silently at/after the
  single next statement `SECVID_TRUST_CNTL <- 0x0` (`[15]` never printed;
  no hangcheck/fault/panic lines). Prior `-22` path is gone; new
  frontier is one statement later.
- GPU_SET OOB itself TIMED OUT this run (`INTR_INFO=0x0`) yet everything
  downstream worked — GPU_SET ack is not required for progress.
- Silent reboot followed (lived 470s+); faults/panics/SMMU: 0.
- Per PART 5: no fix applied; probe stays gated, not permanent.

## PART 5 compliance
Data collected, stopping. Recommended next: instrument around
`SECVID_TRUST_CNTL` (and CP-IRQ handler as alternate suspect), then
decide the permanent home (if any) for the vote.
