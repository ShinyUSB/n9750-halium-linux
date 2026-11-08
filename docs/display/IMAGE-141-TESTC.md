# IMAGE-141 TEST C — GPU_SELFTEST_PASSES (first live workload proof)

## Result
- `SELFTEST137: cp_init ret=0 RPTR=0x12 WPTR=0x12 STATUS=0x1`
  (second CP_ME_INIT in-boot: 0x9 → 0x12, CP fetched and idled twice).
- `[25] ret=0` after. Reset later: `NON_SECURE_WDT` (no storm/faults).
- Device alive on fresh boot.

## Classification: GPU_SELFTEST_PASSES
CPU→ring→CP→idle proven twice in one boot. The GPU is a working
command processor; remaining work is stability (post-init resets),
firmware variance (retry logic), and eventually real submits via Mesa.

## Incidental fixes this round
- Cmdline truncation: bootloader chops the long tail (selftest flag
  arrived empty). a6xx flags now lead the cmdline (141-c).
- "Duplicate attaches": never existed (ATT138 n=1 every boot; last_kmsg
  appends across boots). Closed.

Image 141-c `20aebe81…` (same kernel as 141). SDE 0, DT = 121.
