# Image 117 — full sequence présentes; GMU firmware self-init failed first (BADDFACE)

## Bootstrap answers
- `GPU_DCVS deferred`: 0 (no suppression fired; bootstrap never entered).
- `FW_VERSION`: never sent (blocked earlier). `PERF`/`BW`/`ECP`: never sent.
- HFI RX / IRQ counts: n/a (no HFI traffic this run).
- `hw_init`: not entered. First HWINIT marker: none reached.

## What happened
```
POST_START: fw_start begin
GMU start poll -> INITRES=0xbaddface @37ms (firmware-reported failure)
POST_START: fw_start end ret=-5 (-EIO)
```
Firmware self-init failed before any HFI message; `hfi_start` and the
DCVS gate never executed. Second trigger write produced no new
`POST_START` (attach latched after first attempt).

## Interpretation
- None of outcomes A–D is reachable from this run: the blocker is
  pre-HFI (firmware `BADDFACE`), not PERF/BW/ECP/HWI.
- `BADDFACE`-vs-`BABEFACE` variance predates this image (firmware boot
  flakiness across runs, independent of the restored HFI sequence —
  no HFI bytes were exchanged before the verdict).
- DCVS serialization itself is untested by this run (nothing to serialize
  when firmware fails first); gate code verified present, zero firings.
- Device stable, no panic; gate held `hw_init` off correctly.

Image 117 `2fb509f2edd608874c79e290bf78d4a45e1a010d29d45775dd86037f8ba7cff9`.
SDE 0, DT +0, production untouched. Stopping per single-variable rule.
