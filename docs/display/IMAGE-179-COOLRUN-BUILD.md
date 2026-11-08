# Image 179 — cool-vote survival run (repackage, no rebuild)

## Why
First 178 run reframed everything: CPUs go deaf one by one seconds
post-run (cpu3 TIMEOUT, then the witness itself stalls) while IRQs,
timers, threads, and MMIO all live-probe OK — the signature of
CPUs browning out/freezing under sustained load, not software
wedges. Sustained 675MHz GX with no power management was flagged
as a suspect back in image 158 but never actually tested (158 never
got a clean run). The power/heat theory fits all variance: fast
deaths at low battery (58% run), slower at full charge, suppressed
(GX never voted) living indefinitely.

## Diff
- 178 kernel bit-identical. Cmdline enables `keepalive156=1`
  (re-votes LOW index 1 / 257MHz every 3s from right after the
  probe vote; initial vote stays max for the proven GX-on path).
  Slumber defaults ON, halt OFF.
- Image 179 `179-halium-boot-a6xx-coolrun.img`
  sha256 `44c27e1e207de4ca4b52828581d6eb5d8db83b498f5ee90241678465809ca592`.

## Readout
- Survival >> historical (minutes+) with downclock ACKs → power/
  heat CONFIRMED as the killer's domain; next is real devfreq +
  thermal-aware policy.
- Same death on schedule → power exonerated (again, finally);
  chase firmware-first causes with recovery keeping watch.

No flash performed (needs Download mode); stopping.
