# Image 130 (build-only) — GX power-state instrumentation, zero functional change

## Diff (read-only prints)
- `a6xx_gmu.c` resume: `GMU_PM:` dev name, `gxpd=NULL` note, runtime-enable
  state, `pm_runtime_get_sync` + `clk_bulk_prepare_enable` entry logs.
  (`dev_pm_opp_set_opp`/`set_initial_freq` don't exist in tree — logged
  as absent in report, not added.)
- `a6xx_gpu.c`: `gxstate_snap()` (SPTPRAC/GDSC_OFF/CLK_OFF/RPMHPWR/
  GXVOTE/MXVOTE/GXGDSCR + ON/OFF verdict) at `[0]` pre-GPU_SET, `[1]`
  post-GPU_SET, `[2]` pre-`adreno_hw_init`, `[3]` pre-SQE; SQE write/
  readback pair in `ucode_init`.
- `adreno_gpu.c` `adreno_hw_init`: before/exact-value/after triples for
  `RB_CNTL`/`RB_BASE`/`RPTR_ADDR` via family-generic `adreno_gpu_read`
  (no a6xx include pollution).

## PART 5 audit (source only)
- `pm_runtime_get_sync(gmu->dev)`: PRESENT (resume).
- `pm_runtime_get_sync(gmu->gxpd)`: **MISSING_UPSTREAM_GX_RESOURCE_REF**
  (no `gxpd` member anywhere in tree).
- `dev_pm_opp_set_opp` / `a6xx_gmu_set_initial_freq`: absent (we use
  `set_freq` post-hfi instead).
- `clk_bulk_prepare_enable`: PRESENT.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- SDE/display 0; DT = 121 content; production untouched.
- ramdisk `eb5d2eb5…`; image 62 `e19c71fa…` untouched.
- Image 130 `130-halium-boot-a6xx-gxstate.img`
  sha256 `3c891367a3ead0495dcbd1b6cdddfcb34e86e607e6e1cea4a31289c2c6375294`.

No flash performed; stopping.
