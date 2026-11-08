# Image 152 (build-only) — vendor SPTPRAC wait (BIT3, 10ms)

## The bug (audit)
- Ours: `POWER_CONTROL <- 0x778000` (same value as vendor ✓), then wait
  `(val&0x38)==0x28` with **100µs total** timeout. Status is always
  0xc0/0x80 (bits 3-5 = 0) → condition unsatisfiable + no time anyway.
- Vendor (same SoC + firmware): same write, wait **BIT(3)** with **10ms**.
- Fix: wait-only change (`(val&0x08)==0x08, 100, 10000`). No new writes,
  no sequencing change. If SPTPRAC powers on, GXSTATE/RPMH/firmware
  behavior may change downstream — that is the experiment.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- Packaged WITHOUT hold_boot (isolate sptprac; hold retest separately).
- Image 152 `152-halium-boot-a6xx-sptprac10ms.img`
  sha256 `82dbfcc01c45d3807982a919aeb66c3e82d97346c97e35393ecd267636a9e3fe`.

No flash performed; stopping.
