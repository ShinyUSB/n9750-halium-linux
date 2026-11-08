# Image 138v2 (build-only) — NOC-safe fix for 138's instrumentation fault

## What 138v1 proved (before dying by its own hand)
- `ATT138 n=1`, correct pdev/node; duplicate-OPP warning noted (benign).
- **Reboot cause identified: `TZBSP_ERR_FATAL_NOC_ERROR`** (TrustZone NoC
  fatal → forced reset; no Linux record possible). Last GPU line was
  inside 138's own `prov138` clock/regulator reads at probe time (GMU
  clocks off) — the instrumentation itself caused the NOC fault.
- Generalizes: unclocked/protected HW reads (and the SECVID_TRUST write)
  are TZ-NOC-fatal. All prior silent reboots plausibly same class.

## Fix (instrumentation only)
- File-scope `gmu_clks_live`, set on both successful enables, cleared on
  both disables. `prov138` skips ALL HW reads (regs/regulators/rates)
  unless live; PM/dev/domain lines (software-only) always print.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors. SDE/DT/prod clean.
- Image 138v2 `138-halium-boot-a6xx-bootrace2.img`
  sha256 `de1a9197b0ea1cbf862a4bde326d7add7a4fd605733d1f66b9a50ab8329de030`
  (supersedes v1 `6fb46f50…`, never successfully booted a trigger).

No flash performed; stopping.
