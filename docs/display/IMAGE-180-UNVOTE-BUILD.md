# Image 180 — post-success vote removal (return power to bootloader state)

## Why
Slumber-off tripled survival (9s→29s) without curing: something
ours destabilizes the system on a ~10-30s fuse with or without
slumber. Remaining candidates narrow to OUR votes/rails (ARC
GX/MX, RPMH enables incl. DDR, HFI vote — all left standing after
success for the firmware to act on over seconds). Pristine boots
(bootloader power state, no votes) are stable indefinitely, so
returning to that state post-success is the clean reversal test.
(Note: 178's halt didn't save because votes persist past CM3 halt.)

## Diff (on top of 179)
- Spin thread at first +10s tick: zero GX/MX ARC votes, clear all
  RPMH vote enables, HFI vote to index 0 (`UNVOTE180` with post
  RPMHPWR). Timer-free (sched_clock-gated), once. Later GPU-reg
  reads may wedge on collapsed GX (accepted datum, machine lives).
- Untouched: everything else. Slumber per cmdline (148 default ON
  in this package; unvote runs in the spin thread regardless).

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- Binary contains `UNVOTE180`.
- Packaged with 148 cmdline.
- Image 180 `180-halium-boot-a6xx-unvote.img`
  sha256 `556d9697df79c34a244550469a14ea61c247ece0bf52d5e97535e1256f79e556`.

## Readout
- Survival >> (minutes, pets flowing) → our votes/rails were the
  destabilizer; next is proper vote lifecycle (devfreq + managed
  slumber/resume toward Mesa).
- Instant death at +10s (collapse kills) → votes were load-bearing;
  restore them, look elsewhere (firmware-first).
- Same death regardless → votes exonerated; remaining suspect is
  firmware-autonomous power action independent of votes.

No flash performed (needs Download mode); stopping.
