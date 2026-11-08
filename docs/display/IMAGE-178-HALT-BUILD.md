# Image 178 — pet-step tracing + CM3-halt therapy

## Why
175's skip engaged live yet death came anyway with no pet print:
wedged past the ping, in/after the WDT register access (the only
remaining unbounded op). Slumber-off tripled survival (9s→29s) but
didn't cure: the WDT block becomes unreachable on a timer with or
without slumber. Two questions, one image.

## Diff (on top of 177)
- Watchdog: `PET178: reads done` / `write done` markers around the
  RST write (read-stall vs write-stall vs print-stall discriminator);
  1000-mismatch cap kept.
- New `a6xx.halt_gmu` param (default off): halt GMU CM3 right after
  successful hw_init (clocks on). If firmware autonomy (NoC floods/
  death throes/power games) destabilizes the system, a halted
  firmware can't; GPU stays usable at fixed clocks (no GMU needed
  for CP submits/fences).
- Packaged with halt=1 + slumber=0 (a6xx-first cmdline order
  against bootloader truncation): pure firmware-absence test.
- Image 178 `178-halium-boot-a6xx-halt.img`
  sha256 `0c7e26d3392efe62a86fc5710dfbcd87d70006182ea02aeb5b9665456d449c2d`.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.

## Readout
- Survival extends past minutes (no bark, pets flowing) → firmware
  activity is the destabilizer; static-GPU path to Mesa opens
  (power management later).
- Same death with `HALT178` confirmed + PET178 marking read vs
  write stall → WDT block gating named precisely (hunt its
  power/clock, not the firmware).
- Either way the pet-step markers end the "where" debate for good.

No flash performed (needs Download mode); stopping.
