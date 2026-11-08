# Image 130 — outcome A: GX OFF at all points, CP writes void

## GXSTATE[0..3] (identical)
- `SPTPRAC=0x80 GDSC_OFF=0 CLK_OFF=1 RPMHPWR=0x0 GXVOTE=0x0 MXVOTE=0x0`
  (+ GXGDSCR line in raw log) → verdict **GX_OFF** before GPU_SET,
  after GPU_SET, before ring writes, before SQE. No transition at GPU_SET.

## RINGHW-W pairs
- `CNTL/BASE/RPTR_ADDR/SQE_BASE`: before=0, exact values written
  (`CNTL` computed, `BASE=0x1000`, `RPTR=shadow`, `SQE=0x9000`),
  after=0. Every GX write vaporizes; GMU-side OOB/reads fine.

## Temporal correlation (A)
GX OFF + CP registers zero at all four points → CP writes dropped for
lack of GX clock (rail itself on: GDSC_OFF=0). Very strong evidence.

## Adjacent findings
- `sptprac_enable` waits `(val&0x38)==0x28`; status sticks at `0xc0`
  (`&0x38=0x00`), returns 0 anyway (non-fatal in code, fatal in effect).
- 6th full ACK chain underneath; `CPIDLE-FAIL` → `-22`; silent reboot
  after. Faults/panics: 0. Device alive (69s).
- Next territory: why GMU never drives power (all votes 0x0, SPTP
  0x00≠0x28, GX_HM_CLK stuck) — SPTPRAC/GX-clock bring-up, not ring code.

Image 130 `3c891367a3ead0495dcbd1b6cdddfcb34e86e607e6e1cea4a31289c2c6375294`.
SDE 0, DT = 121, production untouched. Stopping.
