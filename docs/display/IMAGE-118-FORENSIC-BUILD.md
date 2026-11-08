# Image 118 (build-only) — forensic BADDFACE capture, instrumentation-only

## Diff (functional writes: zero)
- `a6xx_gmu.c` only: added file-scope `f118_*` forensic helpers + rewrote
  the `a6xx_gmu_start` pre-SYSRESET/poll block. No init write, reset,
  address, vote, clock, power, MMU, HFI, or retry semantic changed.
- PRE: `f118_snap_regs(PRE)` (SYSRESET/INITRES/BOOTCFG/CM3CFG/WFI/FWBUSY/
  NMI/QTBL/INFO/SFR/CPMSG/G7/FENCE0-1/HFISTAT/G2HINFO/G2HMASK/H2GRAW/
  H2GINFO0/RPMHPWR/KEEPALIVE; G2HRAW noted N/A) + `f118_store_pre`
  (focused ITCM/DTCM into before-arrays). Megabyte PRE dump dropped to
  protect the kmsg ring (dumper kept `__maybe_unused`).
- Poll: same 100us x 10000 + same verdict breaks. Adds `FORENSIC POLL`
  on every INITRES change + samples at ~5/10/15/20/25/30/35/37/40/50ms.
- Verdict (GOOD and BAD, identical path via `f118_verdict`): regs, full
  4KiB LOG + LOGHDR, focused MEM (ITCM 0x0000/0x1e00/0x0200 windows,
  DTCM 0x40000/0x40e00/0x0ff0 windows), `FORENSIC DIFF` (changed dwords
  only, before/after), `FORENSIC SCAN` (deadbeef/babeface/baddface/
  0x6000xxxx hits in focused regions + LOG). Dumps run synchronously
  before `fw_start` returns; gate keeps HFI/hw_init/CP/zap off on failure.
- `a6xx_hfi.c`/HFI sequence untouched (full 117 order intact).

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- SDE/display 0; DT pre-existing 14 only; production untouched.
- ramdisk `eb5d2eb5…`; image 62 `e19c71fa…` untouched.
- Image 118 `118-halium-boot-a6xx-forensic.img`
  sha256 `abc60ecf12a23cbfbf493eced52c067afefbf35eae105f08d14b4dbbffa0758d`.

No flash performed; stopping.
