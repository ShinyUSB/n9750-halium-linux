# Image 171 — wedge finder (LAST instrumentation, then fixes only)

## Why
Slumber-era boots show an impossible signature: trigger passes the
arm print, then SLUM165/[25] never print, yet the write returns and
shots fire. Either pm_suspend wedges silently (all its loops are
jiffies-bounded — shouldn't be possible) or lines are lost. And the
pet path (timer → kthread → IPI ping → WDT MMIO) dies while sibling
timers fire — narrowed to ping vs MMIO but never tested live. This
image tests both directly instead of inferring.

## Diff (on top of 170)
- `pm_suspend`/`stop` step prints (`SSTEP171`): entry, wait_for_idle
  done, VBIF done, notify done, hfi_stop/irq_disable/rpmh_stop/
  clocks-off done. The last one standing names the wedging call.
- Spin thread probes (timer-free): WDT MMIO double-read + latency
  at +2s (`DW161-WDTMMIO`), self-IPI round trip + latency at +3s
  (`DW161-IPI`). A wedge inside either names the pet-killer by its
  absence against proven surroundings.
- Untouched: everything else.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- Binary contains `SSTEP171`, `DW161-WDTMMIO`, `DW161-IPI`.
- Packaged with 148 cmdline (slumber ON).
- Image 171 `171-halium-boot-a6xx-wedgefinder.img`
  sha256 `5876de340a50c6fc43c82a02b14aff733d2d5d4974013d0e0b5fba15268782da`.

## Readout (each decisive)
- Last SSTEP + no SLUM165 → exact wedging call (fix that call).
- WDTMMIO missing/slow → WDT bus stall (fix: power/clock the WDT
  block or move pet path).
- IPI missing/slow → IPI delivery (fix: ping mask/affinity, drop
  wait, per-CPU pet).
- All present + death anyway → pet path fully exonerated; the
  killer is elsewhere entirely (back to firmware-first theories
  with recovery keeping the machine alive to watch).

No flash performed (needs Download mode); stopping.
After this image: fixes only, no new instruments.
