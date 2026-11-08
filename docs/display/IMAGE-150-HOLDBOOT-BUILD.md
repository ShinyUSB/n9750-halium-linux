# Image 150 (build-only) — remove vitals poller + hold-boot-OOB gate

## Why
- Vitals poller never produced data (every death beat it) and reads live
  GX regs on a timer — same hazard class as the 138 NOC self-fault.
  Removed entirely (fn + decls + arming).
- Post-success deaths follow the [23]/[24] OOB handoff (GMU takes power
  management). Test: keep boot OOBs (no handoff), watch survival.

## Diff
- `a6xx_gpu.c`: vitals fn/decls/arming deleted; `hold_boot` gate around
  the two OOB clears (`HOLD150` log when held).
- `a6xx_gmu.c`: `bool hold_boot` + `a6xx.hold_boot` param (placement
  verified UNDER prefix).
- Untouched: everything proven.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- SDE/display 0 (verify at flash); DT/prod untouched (only 2 adreno files).
- Packaged all-flags + hold_boot=1.
- Image 150 `150-halium-boot-a6xx-holdboot.img`
  sha256 `a2ffa3e9d81dbd51e22cf5517fc31f32f9e8c57363e8ba8da45524f7f385a393`.

No flash performed; stopping.
