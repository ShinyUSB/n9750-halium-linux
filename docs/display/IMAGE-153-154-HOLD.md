# Image 153 run + Image 154 (repackage, no rebuild)

## 153 run: death DURING bootstrap (new timing)
- SPTPRAC 100ms wait: still TIMEOUT (`0xc0` stuck this run — rail timing
  varies run to run: 26ms vs >100ms).
- Version/PERF/BW ACKs → ECP entered → reset DURING the ECP wait
  (<5s after BW ACK). No NACKs, no storm, no [25], no panic.
- First pre-hw_init death: handoff theories untouched (nothing handed
  off yet). Killer acts as early as ECP phase.

## Refined discriminator (all runs)
- hw_init ENTERED ⟺ death follows (minutes or seconds).
- hw_init never entered (suppress) ⟺ device lives indefinitely.
- 113 (no hw_init): stable. 115 (hw_init hang): rebooted. Holds cleanly.

## 154 (no rebuild: 153 kernel + hold_boot=1 cmdline)
- Tests whether skipping the OOB power handoff extends post-success life.
- Image 154 `154-halium-boot-a6xx-holdtest.img`
  sha256 `934879d784b7b8f99693151949efd76d18be8bfdcd0468944bd5cc5198319027`.

No flash performed; stopping.
