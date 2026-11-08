# Image 127 — GBIF outcome UNKNOWN (evidence lost); NACK storm + reader survives

## What happened
- Trigger ran (~25s). Host stream broke ~10s later (`/dev/kmsg: Broken
  pipe`, 6 lines captured: `fw_start begin` + GOOD only).
- Firmware entered a sustained NACK storm: continuous size-3 error
  packets (ids cycling 0/64/192, firmware seq now 3400+, ~thousands/sec,
  `HFI-ACK ... error=111` each, no waiter matches). Still ongoing at
  uptime 223s.
- Storm rotated the entire dmesg ring (holds ~seconds); all
  PERF/BW/GBIF/HWINIT history from this run is unrecoverable from the
  device. **Whether GBIF[00..04] executed is UNKNOWN.** Methodology
  failure owned: single fragile stream + maximal per-packet logging
  under a storm.

## Reader validation (strong, unexpected)
- 4,830,000+ `kgsl_hfi_irq` and counting, zero panics. The image-125
  alignment walks every padded packet (`raw_end=371 aligned=372`, …).
  The 124 `BUG_ON` would have died in seconds; the parity fix holds
  under a multi-million-IRQ storm. `kgsl_gmu_irq` = 0.
- SMMU faults/panics/Oops in retained ring: 0. Device alive, SSH
  responsive at 223s.

## Storm packet shape (sampled live)
- `HFI_RX index=N hdr=0xd5X103yy id={0,64,192} type=1 size=3 seq=34xx`,
  advance `+3/+1-align` each. High header bits now set (`0xd5..`)
  vs early runs (`0x00..`) — recorded, not interpreted.

## Next step (no new image needed)
Image 127 is still flashed. Reboot (manual), then re-trigger with a
self-healing host stream (retry loop) so markers are captured live even
if the storm recurs. The storm itself may or may not recur — that is a
datum either way.

Image 127 `374e64ee470254eec66b9ef9ca2237ae0b98c91ba100b112346254e9bfdf94e3`.
SDE 0, DT = 121, production untouched. Stopping.
