# Image 129 — RINGHW exposes the real fault: RB regs read 0 after programming

## Runtime datum (the smoking gun)
- Full ACK chain again (5th: 1/4/3/11/14/10, error=0) with NACK storm
  interleaved (reader walks both, no panic).
- `RINGHW[00] SQE_INSTR_BASE: iova=0x9000 hw=0x0`
- `RINGHW[01] CP_RB_BASE: iova=0x1000 hw=0x0`
- `RINGHW[02] CP_RB_CNTL hw=0x0`, `[03] RPTR_ADDR hw=0x0`.
- `rb_size=32768 sw_wptr=0`; ring0[0..15] = valid `CP_ME_INIT`
  (`70c80008 0000002f ...`). CPU-side ring is perfect.
- `CPIDLE-FAIL` again (RPTR/WPTR 0) → `-22`. No faults/panics (0).

## Analysis
- Ordering premise refuted twice: HW programming already precedes SQE,
  and the writes verifiably do not stick (read back 0 immediately).
- `reg - 1` accessor alarm investigated and cleared: matches upstream
  (`+1`/`-1` cancel; correct a6xx table installed; `reg_check` would
  BUG otherwise — none fired).
- `rb_iova=0x1000` is plausibly legitimate (low aspace allocator start);
  mapping is not the prime suspect — the zeroed REGISTERS are.
- Standing contradiction: GBIF_QSB readbacks returned written values
  (127 rerun) through the same aperture/accessors, while CP_RB/RBBM
  read 0. Hypotheses for next image: (a) GX power/clock domain not up
  during RB programming (rails: `gfx_rail_on` skipped, SPTPRAC always
  0xc0-timeout, RPMHPWR=0x0 at BABEFACE); (b) RB regs need an enable/
  latch bit our CNTL value lacks; (c) immediate-write-vs-later-read
  comparison to isolate (program-then-readback inside `adreno_hw_init`
  itself + GX power-state snapshot during hw_init).

Image 129 `b9966ae52872f4d4ba174166ad67050d4596118bc49d6836dc15fe9a54245d41`.
SDE 0, DT = 121, production untouched. Stopping.
