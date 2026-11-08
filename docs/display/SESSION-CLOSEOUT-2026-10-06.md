# Session close-out (2026-10-06) — GPU activity ⟺ death, proven both ways

## The closing experiment (same boot, both halves)
- Pristine 12+ min: alive, healthy, zero GPU lines.
- Trigger → full ACKs → vote ACK → selftest 10/10 → `[25] ret=0` →
  `NON_SECURE_WDT` reset within ~2 min.
- With earlier data: pristine/suppressed boots live indefinitely;
  post-success boots die. Single ambiguous pristine-NOC case stands
  against 5+ clean contrasts. Link is airtight enough to build on.

## What is proven and parked
- Bring-up complete and repeatable (firmware lottery aside, retry covers).
- DCVS writes = GX switch. SPTPRAC slow (26ms+). GPU_SET unnecessary.
- BW table unnecessary. SECVID skip required (TZ trap). Zeroed pages
  Correlate with calmer firmware (no NACK/BADDFACE since).
- SMMU: routing correct; 3 rare firmware-garbage faults, now non-fatal.
- Mesa staged (renderD128 + msm_dri.so); CP IRQ path never verified live.

## Queued next (in order)
1. Post-success survival: hold_boot retest with success (150 flashed
   image still valid), then slumber-handshake vs keepalive-contact.
2. CP IRQ verification (fence completion via interrupt, not polling).
3. Mesa surfaceless proof (eglinfo/pbuffer) on a surviving run.
4. ZAP blobs (needs stock dump from user) — only for protected content.
5. `a6xx_selftest` rename (stuttered identifier) + log-filter upkeep.

Device left fresh (rebooted post-run). Image 160 flashed.
SDE/display/BT/WiFi untouched throughout. Baseline 62 preserved.
