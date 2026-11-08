# Image 166 — no more self-inflicted panics + GMU fault recovery

## Why (the archived oops that reframes everything)
Crash archive yielded a full `Oops - BUG` with PC in `a6xx_gmu_irq`:
after a successful run, CPU0 idle in deep `lpm_cpuidle_enter` took a
GMU IRQ with `WDOG_BITE` set and hit the image-139 TEMPORARY `BUG()`
("Temporary until we can recover safely") — a deliberate panic on a
firmware event, never removed. So the GMU firmware runs its own
watchdog, it expires seconds-to-minutes post-boot, and our handler
answers by killing the machine. Unified model: bite + IRQs enabled →
BUG panic; bite + IRQs masked (post-slumber) → firmware death throes
wedge the pet path → Linux barks 11s later → WDT reset; fast class →
hard wedge before bark matures.

## Diff (on top of 165)
- Both IRQ-handler `BUG()`s → loud state dump (kept) + schedule
  process-context recovery. No behavior change on the happy path.
- New `recover166` work: force off (KEEPALIVE=0, hfi_stop,
  irq_disable, sptprac_disable, fence allow, CM3 SYSRESET, rpmh_stop)
  then cold re-bootstrap via the proven `a6xx_gmu_resume` (which
  retries the firmware lottery itself), under the DRM mutex. Failure
  stays alive + logs (suppress philosophy).
- Kept: slumber lifecycle, spinwatch, DW ladder, clocks.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- Binary contains `RECOVER166`, `scheduling recovery`, `SLUM165`.
- Packaged with 160 cmdline.
- Image 166 `166-halium-boot-a6xx-recover.img`
  sha256 `1dfcf73810fcb0b7b586e4b513cdde6049c110c7a0bfb579205986b89980c9fe`.

## Readout
- `RECOVER166: end ret=0` + survival → panic class eliminated;
  repeated recoveries map the firmware-bite cadence (next: prevent
  bites via votes/slumber-contact).
- Recovery itself wedges → its last line names the exact call.

At flash: add `RECOVER166|WDT139` to the capture filter. No flash
performed (needs Download mode); stopping.
