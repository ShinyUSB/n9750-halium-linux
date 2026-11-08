# Image 129 (build-only) — NO reorder; ring HW already precedes SQE

## Premise correction (verified in source)
The presumed bug does not exist: `adreno_hw_init` programs
`CP_RB_CNTL`/`CP_RB_BASE`/`CP_RB_RPTR_ADDR`, then `a6xx_ucode_init`
programs `CP_SQE_INSTR_BASE` — all BEFORE `CP_SQE_CNTL=1` and
`a6xx_cp_init()`. The `[21]` label was software-only (`cur_ring`
assignment), which misled the task premise. No block moved, nothing
duplicated (per the task's own single-site rule).

## Diff (`a6xx_gpu.c`, read-only diagnostics)
- `RINGHW[00..03]`: SQE_INSTR_BASE / RB_BASE / RB_CNTL / RPTR_ADDR
  (iova-vs-hw readbacks via existing `gpu_read64`/`gpu_read`).
- `RINGHW`: rb_iova/size/sw_wptr. `RINGHW[04]`/`[05]` around SQE/cp_init.
- `CP_PRE_FLUSH` / `CP_POST_FLUSH` snapshots in `a6xx_cp_init`.
- `CPIDLE-FAIL` extended with ring0[0..15]. Same `-EINVAL`, no behavior
  change. All prior fixes retained.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- SDE/display 0; DT = 121 content; production untouched.
- ramdisk `eb5d2eb5…`; image 62 `e19c71fa…` untouched.
- Image 129 `129-halium-boot-a6xx-ringhw.img`
  sha256 `b9966ae52872f4d4ba174166ad67050d4596118bc49d6836dc15fe9a54245d41`.

No flash performed; stopping.
