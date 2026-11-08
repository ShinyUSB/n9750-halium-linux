# Image 165 REMADE — first managed power cycle (slumber + spinwatch)

## Why remake (research-driven, not another minute-adjustment)
Upstream freedreno history + current `torvalds/master` audited:
- Skipped `prepare-slumber` during suspend leaves stale RPMH votes and
  breaks the system (freedreno patches 2022/2025) — the slumber
  handshake is load-bearing upstream, and our tree NEVER executes it
  (suspend path exists but runtime PM is off, so it never runs).
- Upstream `a6xx_gmu_shutdown`: dummy GPU_SET handshake if needed →
  wait_for_idle → bus_halt → notify_slumber (PREP_SLUMBER HFI or OOB)
  → poll BUSY clear → hfi_stop → irq_disable → CM3 SYSRESET →
  rpmh_stop. Our in-tree `a6xx_gmu_stop` matches this shape (minus
  CM3 halt and bus_halt, which need absent drivers) and was verified
  fully bounded (all polls jiffies/timeout-limited).
- Already in-tree and verified present: CP_MSG log buffer (img 102),
  ICACHE/DCACHE_CONFIG + INTER_FRAME_CTRL (power_config), RSC
  start/stop, HFI at dummy-skipping layout upstream (ours still at
  dummy slot — bootstrap works, NOT touched).
So the single biggest honest step: EXECUTE the existing suspend path
after success instead of leaving the GPU awake at the firmware's
mercy. This subsumes hold_boot/keepalive/cool-vote theories into the
upstream-blessed lifecycle and is required for real graphics anyway.

## Diff (on top of 164)
- `a6xx_gmu.c`: `BUG_ON(ret)` in `a6xx_gmu_stop` → log + error return
  (never BUG in the trigger path); new `a6xx.slumber165` param,
  default ON (`0` restores stay-awake behavior).
- `a6xx_gpu.c`: after selftest success + instrument arming, call
  `a6xx_pm_suspend()` on success; log `SLUM165` with post-slumber
  RPMHPWR/BUSY; on failure continue awake (old behavior, instruments
  live). SHOTN1 keeps capturing the awake baseline; SHOT0 verifies
  slumber (RPMHPWR should read 0xf).
- Kept: spinwatch kthread (timer-free +1s/+5s witness), DW ladder,
  clock triple, all 160–164 behavior.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- Binary contains `SLUM165`, `slumber165`, `DW161-SPIN`.
- Packaged with 160 cmdline.
- Image 165 `165-halium-boot-a6xx-slumber.img`
  sha256 `55235edc4f7e02dd3cbef6a4c9b186d4d6e4e775866111a97c7fc55dd8db77dc`.
  (Supersedes the spinwatch-only 165 build `5e16bfdc…`, not flashed.)

## Readout
- Survival >>11s with `RPMHPWR=0xf` → power-lifecycle CONFIRMED as
  the killer's domain; next is resume-on-submit (warm boot) toward Mesa.
- Slumber fails / dies on schedule → lifecycle exonerated; spinwatch
  +5s dump still delivers the pet-kthread wait point.
- Either way the next image is decided by data, not guessing.

At flash: add `SLUM165` to the capture filter. No flash performed
(needs Download mode); stopping.
