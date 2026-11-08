# Image 176 — force the pet through unstable reads

## Why
175's skip engaged live (`cpu 1 no IPI ack (skipped)`) yet the bark
still came 1.5s later with no pet print: wedged past the ping, in
`pet_watchdog`'s read-stability loop (the only remaining unbounded
construct — every other link live-probes OK). A faulting bus
returning garbage makes consecutive reads differ forever (spin);
a stalled bus would wedge the read itself (indistinguishable here,
covered by the variance probe below).

## Diff (on top of 175)
- Watchdog: cap the stability loop at 1000 mismatches, then force
  the RST write + loud log. A wedged loop becomes a logged forced
  pet; a stalled bus still wedges in readl (named by absence
  against the variance probe).
- Spin thread: 1000-rapid-read variance probe (`DW161-WDTVAR`:
  min/max/transitions). Healthy 32kHz counter: 1-2 values.
- Untouched: everything else.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- Packaged with 148 cmdline (slumber ON).
- Image 176 `176-halium-boot-a6xx-forcepet.img`
  sha256 `82db78380c13434498a9cbbbc0c2c73d3d867ce323cb6c9ac6cdf02f66a027a02`.

## Readout
- Pets flow past bark window (forced or clean) → survival; the
  read loop was the killer, now capped.
- `forcing pet` + death anyway → RST writes don't reach hardware
  (bus stall class) — next: WDT power/clock audit.
- `WDTVAR` garbage spread → NoC fault class identified live.

No flash performed (needs Download mode); stopping.
