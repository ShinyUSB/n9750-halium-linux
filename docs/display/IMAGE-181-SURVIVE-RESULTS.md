# Image 181 run — survivability proven, new wedge named

## What 181 proved (both halves on the same flash)

- First trigger (16:38 host, t+60.6s device): `on-demand GPU core load
  requested` → `SELFTEST142: done last_iter=10 ALL 10 PASS` → phone
  stayed up 500+ seconds with normal charger kicks, WiFi roam
  (`ROAM_LOG_CMPL` t+495.9s) and PCIe suspend/resume cycling. Under
  every image ≤180 that sequence ended in `NON_SECURE_WDT` within ~11s.
- `watchdog_v2.enable=0` arrived intact (`/proc/cmdline` shows it,
  `MSM Watchdog Initialized` count 0, zero `[pet_watchdog]` lines,
  zero `BARK181`, zero bites). The WDT fuse is gone.
- No `NON_SECURE_WDT` anywhere. 181's core claim holds: the apps WDT
  was the fatal surface and it is now disarmed.

## What 181 did NOT fix (and misread during the session)

- The trigger ran synchronously in the sysfs writer, so the operator's
  ssh call blocked in `vfs_write` for the whole bring-up and the 60s
  client timeout killed it mid-write. The "phone is dead" read was
  wrong twice over: the work completed on-device, and the ARP `FAILED`
  for `.16` was the t+495.9s WiFi roam, not a crash (RESUME.md already
  warned the IP can move; the check was skipped).
- Post-success `pm_suspend` still wedges: `SSTEP171: pm_suspend enter`
  with no `wait_for_idle done` / `vbif halt done` (80th consecutive
  instance). New precision from the 16:56 archive: the trigger thread
  (`sh` pid 1241) is parked in `printk` called from
  `a6xx_pm_suspend+0x4c` → `console_unlock` → `wake_up_klogd`, with RCU
  stall age growing monotonically (t=6302 → 63017 jiffies) across 7
  dumps from t+285s to t+474s. CPU3 never runs again: SSH dies, OTG
  keyboard dies, weston keeps ticking. The rest of the system
  (charger, dhd, five-diag) logs normally throughout.
- The 16:56 boot ended at ~t+559s with `UploadCause[KERNEL PANIC]`
  (`panic_msg =  PC =  `, both empty) via the sec_debug dump path
  (`print_aop_core_dump`, `LogFlush:SecDebugLog Flushing`, `UEFI End`,
  `pm_restart) rebooting...`). No panic, oops, BUG, `FPUT timeout` or
  `Software Watchdog` line exists anywhere in the ring. The empty
  message + wedged console are consistent with a panic path that could
  not emit (console held by the wedged writer), but the exact panicker
  is unidentified. `/dev/watchdog0` (softdog) had no openers, ruling
  softdog out. `CONFIG_SEC_DEBUG_FPUT_WATCHDOG` (180s) and
  `CONFIG_SOFT_WATCHDOG` remain armed-by-default suspects only.

## Standing verdicts carried into 182

1. WDT exonerated as a reset vector (disarmed, zero events over two
   bring-ups). `NON_SECURE_WDT` is closed.
2. The failure mode is now *unresponsiveness*, not reset: a wedged
   printk in the trigger thread pins CPU3 and hides a live phone.
3. `pm_suspend` after success is still the wedging call (printk first,
   VBIF poll second). Mesa needs the GPU awake anyway, so slumber is
   the wrong default for the render milestone.
4. A second reboot vector exists past the WDT (sec_debug dump at ~295s
   post-trigger). Fuses to pull: softdog, FPUT watchdog.

Image 181 `ec62e50d…`. SDE 0, DT = 121, prod untouched.