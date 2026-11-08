# RUN-140-APSS-WDT-FORENSICS.md — STOPPED: params unsupported

## Classification: B) APSS_WATCHDOG_BITE_WITHOUT_LINUX_BARK

## STOP (task step 3)
- `msm_watchdog.appsbark`: ABSENT tree-wide.
- `msm_watchdog.print_all_stacks`: ABSENT tree-wide.
- Active driver: `drivers/soc/qcom/watchdog_v2.c` (params: `enable`,
  `WDT_HZ`, `ipi_en` only). `CONFIG_QCOM_WDT` is not set — no second driver.
- No cmdline change made, no flash, no rebuild. Nothing to boot.

## Driver reality (source-verified)
- Linux DOES register the bark path: per-CPU PPI or `apps_wdog_bark` IRQ
  + `configure_bark_dump` + `sec_debug` summary. A Linux-visible bark
  would print `Watchdog bark!` + dumps + petetime accounting.
- Such lines appear in ZERO retained buffers across dozens of boots —
  while `[pet_watchdog]` lines show healthy advancing pets throughout.
- GMU-side watchdog ruled out independently: its bite path is
  `dev_err + BUG()` (would panic WITH record); never observed.

## Interpretation (B, both hypotheses retained)
- TZ reports `NON_SECURE_WDT` while Linux (a) pets normally, (b) never
  runs its bark handler. Either the bark is routed to TZ despite Linux
  registration, or the biting watchdog is one Linux does not pet
  (pets advancing on the apps WDT do not preclude it).
- C rejected (reset reason IS present). D rejected (no other source:
  GMU WDOG self-reports; qcom-wdt unbuilt). E partially true
  (requested params absent) but B classifies the event, not the driver.

## Captured without new runs (existing buffers)
- Pet markers: healthy to end in all runs (`last_count` advancing).
- Reset reasons: `NON_SECURE_WDT` ×3 post-success, `NOC_ERROR` ×2
  (one self-inflicted, one SECVID-class). Zero `Watchdog bark!` lines.
