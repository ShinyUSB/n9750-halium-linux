# Image 174 — pet-phase tracing + firmware log + blind-window cover

## Why
Cleanest crash yet isolated the pet path as the SOLE casualty
(timers/threads/MMIO/IPI-self all live-probed OK; only cross-CPU
IPI untested and it wedged twice on varying CPUs). Three gaps left:
(1) which pet phase wedges (timer wait vs ping vs MMIO),
(2) the +5s→+13s blind window (pet due, then bark),
(3) the firmware's own log buffer (written since img 102, never read).

## Diff (on top of 173)
- Watchdog kthread: `PET174: woke` after timer wait + `PET174: ping
  done` with alive mask per cycle (9.36s cadence, rotation-proof).
  Next missed deadline shows the exact wedged phase.
- Spinwatch to +15s with compact `SPIN12` snapshot (+12s regs/clocks/
  online); +15s completion line.
- New `gmu_log` debugfs node dumping the GMU firmware log buffer
  (read-only host RAM, safe on dead GMU) for live reads post-run.
- Untouched: everything else.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- Binary contains `PET174`, `SPIN12`, `SPIN15`, `GMU log:`.
- Packaged with 148 cmdline (slumber ON).
- Image 174 `174-halium-boot-a6xx-petrace.img`
  sha256 `5830764f48f9dbbad400fce96cb4f68eb80dc47fa191dc0bcdebc3c48f459e11`.

## Readout (each final for its branch)
- Last PET174 phase + no RST → exact wedged link (fix that link).
- `gmu_log` shows firmware panic/error pre-bite → firmware-first
  cause named in its own words.
- SPIN12 state vs death → blind window closed.

No flash performed (needs Download mode); stopping.
