# Image 126 — set_hwcg exonerated; first failure inside VBIF-start pair

## Bootstrap (2nd full-ACK run)
`FW_VERSION→ACK`, `PERF→ACK`, **`BW→ACK(3)`**, `ECP→ACK(11)`,
`core→ACK(14)`, `start→ACK` (seq 0–5, all error=0). `hfi_start ret=0`.

## Marker walk (outcome C: hwcg completes, VBIF hangs)
```
[00] enter
[03] GBIF/HALT absent (noted)
[01]/[02] GPU_SET returned (0x80010000)
[05]/[06] SECVID ok
[07] + ADDR64[00..11] ABSENT (confirmed on-device)
[08] empty region, STATUS=0x0 STATUS3=0x0
[09] before set_hwcg
[10] after set_hwcg (+61us) STATUS=0x0 — hwcg COMPLETE, not the blocker
[11] before VBIF start
... silent reboot, no panic/Oops/BUG. [12] never printed.
```
First operation not completed: one of
- `VBIF_GATE_OFF_WRREQ_EN <- 0x9`, or
- `RBBM_VBIF_CLIENT_QOS_CNTL <- 0x3`.
Reads work throughout (STATUS readable, GPU idle 0x0) — death is on a
VBIF write or its immediate effect, not a dead bus.

## Remaining items
- SMMU faults: none logged. Panic: none (watchdog-style silent reboot).
- Device rebooted, now alive on fresh boot.

Image 126 `a8dfd305807038e6fc54fca1a637fbfff4760fba316336210f69416ba40cfd78`.
SDE 0, DT = 121, production untouched. Stopping.
