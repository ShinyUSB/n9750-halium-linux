# Image 118 — GOOD run (BABEFACE ~1ms); full canonical HFI re-tested

## Verdict
- GOOD: `INITRES 0 -> 0xbabeface` at poll ms~1 (single transition, no
  intermediate values, no milestone samples hit). No `baddface`.
- Contrast with Image 117 BAD run: `0xbaddface` at ~37ms. Timing
  asymmetry recorded, no semantics assigned.

## PRE snapshot (before SYSRESET release)
```
SYSRESET=0x1 INITRES=0x0 BOOTCFG=0x2 CM3CFG=0x4052 WFI=0x0 FWBUSY=0x0 NMI=0x?
QTBL=0x60001000 INFO=0x1 SFR=0x6040100 CPMSG=0x60006000 G7=0x1
FENCE0=0x80a008a0 FENCE1=0x0 HFISTAT=0x0 G2HINFO=0x0 G2HMASK=0xff7ffffe
H2GRAW=0x0 H2GINFO0=0x0 RPMHPWR=? KEEPALIVE=? (in raw log)
G2HRAW=N/A (no such reg in this GMU)
```

## GOOD register snapshot (at BABEFACE)
```
SYSRESET=0x0 INITRES=0xbabeface BOOTCFG=0x2 CM3CFG=0x4012 WFI=0x0
FWBUSY=0x0 NMI=0x0 QTBL=0x60001000 INFO=0x1 SFR=0x6040100
CPMSG=0x60006000 G7=0x1 FENCE0=0x80a008a0 FENCE1=0x0
HFISTAT=0x1 G2HINFO=0x0 G2HMASK=0xff7ffffe H2GRAW=0x0 H2GINFO0=0x0
RPMHPWR=0x0 KEEPALIVE=0x0
```
Note: CM3CFG `0x4052 -> 0x4012`, HFISTAT `0 -> 1` (firmware-side writes,
recorded not interpreted).

## Firmware log (full 4KiB; only 5 non-zero lines, rest zero)
```
LOG+000 00000021 0242acab 00000001 abcdabc0
LOG+010 00000023 0242ad1e 00000001 deadba55
LOG+020 00000023 0242ad96 00000002 deadba55
LOG+030 00000023 0242ae02 00000003 deadba55
LOG+040 00000023 0242ae42 0000003a 00000000
LOG+050..FFF zero
```
Log write-ptr: no such register in this GMU (stated in LOGHDR).

## Focused before/after diff (changed dwords only)
- ITCM: 128 words, single pair `before=0000053d after=00002ca9`,
  range `ITCM+0240..033c` (inside the 0x0200..0x0380 window). No other
  ITCM window changed (0x0000.., 0x1e00.. silent except the scan hit below).
- DTCM: 49 changed words: flag-style `0x0X -> 0x8XXXXXXX/0x1XXXXXXX`
  (e.g. `40014: 00000008->80000008`, `40088: 00000011->80000011`),
  counters (`400c0..400cc`), pointer pair `40100/4010c: 0->60006000`,
  `40e4c..40ffc` stride-0x10 chain (`fffc0010`-class values), plus
  `DTCM+0ffc/100c` status words. Full list in `kmsg-forensic.log`.

## Magic scan hits (no semantics assigned)
```
ITCM+0x24 = 0xdeadbeef   (known PSP poison, unchanged)
ITCM+0x1ed8 = 0xbaddface (constant in code, not a status)
DTCM+0x40100 / 0x4010c = 0x60006000 (log/CP_MSG pointers)
LOG: no deadbeef/babeface/baddface/6000xxxx words (deadba55 only)
```

## HFI follow-through (canonical 117 sequence, first GOOD-then-HFI run)
```
fw_start ret=0 -> BOOTSTRAP BEGIN
FW_VERSION -> ACK (id=1) ret=0
PERF_TABLE (v2) -> ACK (id=4) ret=0
BW_TABLE (canonical 110) -> TIMEOUT -110
BOOTSTRAP END; hfi ret=-110; hw_init suppressed, device alive
```
Zero `GPU_DCVS deferred` lines: with no async DCVS caller in-tree, the
gate never fires in either direction. BW timeout reproduces Image 110
exactly (version+perf ACK, BW silent) — deterministic payload/state
rejection, not a race.

## Repeatability
Second trigger write on the same boot produced no new `POST_START`
(attach latched after first attempt) — single sample per boot.

Image 118 `abc60ecf12a23cbfbf493eced52c067afefbf35eae105f08d14b4dbbffa0758d`.
SDE 0, DT +0, production untouched. Stopping.
