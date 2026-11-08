# Image 116 — hw_init never entered; version timed out (new variance)

## HWINIT answers (all negative — expected, hw_init gated off)

- Last HWINIT marker: none. First not reached: `[00]` (hw_init never entered).
- GPU_SET bits (recorded, not executed): req=16, ack=24, clear=24.
- CP_SQE_CNTL / cp_init / zap / adreno_hw_init: none reached, no returns.
- Gate held (`hw_init suppressed … staying alive`); device alive, no panic.

## New variance: version timeout + DCVS race

```
fw_start ret=0 (GMU booted, handshake ok)
HFI: verify_fw_version enter -> TIMEOUT (-110)   <- worked in 5 prior runs
hfi_start (messages) ret=-110
oob GPU_DCVS ack-wait enter -> TIMEOUT            <- freq path in same window
gate suppresses hw_init; system stays alive
```

A `GPU_DCVS` OOB (devfreq/freq path, format differs from our markers) fired
inside the bootstrap window. Whether its GMU interaction raced the version
wait is undetermined — but this is the first run where version itself got no
answer, after many consecutive successes with identical code.

Image 116 `1008b347e9c3f7833e9d1ff910500708a7008fdfe8d258ea822ce0ca863092a1`.
SDE 0, DT +0, production untouched. Markers only; stopping.