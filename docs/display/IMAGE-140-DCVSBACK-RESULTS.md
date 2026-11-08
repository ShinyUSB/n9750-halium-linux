# Image 140 — DCVS-register hypothesis CONFIRMED on first test

## Result
- Full ACK chain + vote ACK + restored DCVS writes → **all RB/SQE writes
  STICK** (`CNTL=0x20c`, `BASE=0x1000`, `SQE=0x9000`), `CP_POST_FLUSH
  RPTR=WPTR=0x9 STATUS=0xc00015`, **`[25] return ret=0`**.
- DCVS OOB still `-110` (irrelevant, as predicted); GPU_SET OOB `-110`
  (irrelevant, as before). BC132 breadcrumbs + POSTINIT trace clean.
- Reset: `NON_SECURE_WDT` (4th post-success instance). Zero `error=111`
  (no storm). No faults/panics.

## Reading
Sticky ⟺ (vote ACK + DCVS writes) now holds 5/5 sticky runs against 1/1
void run on the restored path. The GX power switch is the DCVS
perf/bw register pair as consumed by GMU firmware — not the OOB
handshake, not the HFI vote alone, not boot timing.
Remaining variance is purely firmware-side (BADDFACE/NACK/silence).

## New frontier (unchanged shape)
Post-`hw_init` survival: silent non-secure-WDT reset minutes after
success, with zero kernel record. Next: what the live system does to
the powered GPU (devfreq/userspace/hangcheck/power-collapse).

Image 140 `8899403a…`. SDE 0, DT = 121, production untouched. Stopping.
