# Image 125 — reader fixed; FIRST full HFI bootstrap + new hw_init frontier

## Queue reader (A: verified clean)
- Ring walk this run: `0→4 (aligned=4)`, `4→7 (aligned=8)`,
  `8→11 (aligned=12)`. Every `aligned==raw_end` except the padded
  PERF-ACK (`7→8`), exactly as designed. No OVERSIZE, no panic.
- No NACK burst this run (single ACKs) — burst readability untested,
  but the framing that killed 124 is now walked by construction.

## Bootstrap (B: full ACK chain, first time ever)
- `FW_VERSION→ACK(1)`, `PERF→ACK(4)` (real votes, `gpu=7/gmu=2`),
  **`BW→ACK(3)`** (canonical 110 table!), `ECP→ACK(11)`,
  `core_fw_start→ACK(14)`, `start→ACK(10)`. `hfi_start ret=0`.
- Full sequence in `bootstrap-full-ack.log` (31 lines).
- Causality note: a reader-side fix cannot make firmware answer; the
  PERF-ACK/BW-ACK is firmware-side variance (123/124 NACKed same bytes).
  What 125 guarantees: when firmware DOES answer, the host survives.

## hw_init frontier (new)
- `set_freq done` → `hw_init entered` → `[00]→[03]→[01]→[02] GPU_SET
  returned (0x80010000)` → `[05]→[06] SECVID ok` → `[07]` → silent
  reboot, no panic/Oops/BUG on record. Hang is strictly AFTER [07]
  (clockgating/VBIF/UCHE/CP-protect/adreno_hw_init region).
- IRQ counts: exact numbers unavailable (no timeout path taken; ring
  walked purely). SMMU faults: 0 pre-reboot.

Image 125 `7c55f6e27cd9454b18099fc40cd2ede8f7fd33657abe95f3bf8c5245281bc32d`.
SDE 0, DT = 121, production untouched. Stopping at the new frontier.
