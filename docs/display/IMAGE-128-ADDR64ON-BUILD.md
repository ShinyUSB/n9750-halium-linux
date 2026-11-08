# Image 128 (build-only) — ADDR64 restoration + cp-idle failure snapshot

## Exact diff (`a6xx_gpu.c` only)
- hw_init after SECVID, before `set_hwcg()`: the 12 upstream-ordered
  `gpu_write(..., 0x1)` ADDR_MODE_CNTL writes (CP/VSC/GRAS/RB/PC/HLSQ/
  VFD/VPC/UCHE/SP/TPL1/SECVID_TSB) with `ADDR64[00..11]` + `[07] BEGIN` /
  `[08]` markers. No sleeps/polls/readbacks/reorders; GBIF/CP/HFI/OPP/
  MMU/clocks/power/OOB/DCVS/firmware untouched. Prior fixes retained.
- `a6xx_cp_init` failure path only: `CPIDLE-FAIL` snapshot
  (RBBM_STATUS/INT_0/ CP_RB_RPTR/WPTR), same `-EINVAL` return.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- SDE/display 0; DT = 121 content; production untouched.
- ramdisk `eb5d2eb5…`; image 62 `e19c71fa…` untouched.
- Image 128 `128-halium-boot-a6xx-addr64on.img`
  sha256 `20a95a9fbb182775837f9b29652d58d32d7fac2b36b94b0abeac104d105c3c73`.

No flash performed; stopping.
