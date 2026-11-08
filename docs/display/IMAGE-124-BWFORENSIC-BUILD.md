# Image 124 (build-only) — BW forensic instrumentation, no functional change

## PART 1 — vendor KGSL audit (ShinyKernel, read-only)
- `hfi_send_bwtbl()` (`kgsl_hfi.c:521`): sends cached `gmu->hfi.bwtbl_cmd`
  (header + `H2F_MSG_BW_VOTE_TBL`), no construction at send time.
- `build_rpmh_bw_votes()` (`kgsl_gmu.c:794`): per-usecase TCS cmds from
  `msm_bus_scale_query_tcs_cmd_all()`; addrs+waitmask from usecase 0 only.
- `gmu_bus_vote_init()` (`kgsl_gmu.c:893`): DDR votes from `gmu->pcl`
  (`num_bwlevels`), CNOC votes from `gmu->ccl` (`num_cnocbwlevels`), then
  `build_bwtable_cmd_cache()`.
- `num_bwlevels` = grp3d bus-table usecases = **13** (`gmu_gpu_bw_probe`,
  `kgsl_gmu.c:1156`); `num_cnocbwlevels` = cnoc-table usecases = **2**
  (`gmu_cnoc_bw_probe`, `kgsl_gmu.c:1176`).
- Concrete addrs/data are **runtime TCS resolutions — not statically
  determinable** (reported UNKNOWN, not inferred).
- DT feeds: GPU node `qcom,msm-bus,name="grp3d"` (13 vectors, src 26→dst 512)
  → pcl/DDR; GMU node `qcom,msm-bus,name="cnoc"` (2 vectors, 26→10036) → ccl.
  Struct `hfi_bwtable_cmd` (`kgsl_hfi.h:236`).

## Instrumentation diff (ours, logging only)
- `a6xx_gmu.c`: `bw124_gmu_irqs`/`bw124_hfi_irqs` entry counters.
- `a6xx_hfi.c`: `BW124-SEND` (all msg words in active levels, sizeof,
  q0/q1 before), `BW124-Q` (after + header/size/ret), `BW124-TIMEOUT`
  (q1 rd/wr, both IRQ counts, G2HINFO; G2HRAW noted absent), `BW124-PKT/
  WORD/ACK` ring-span decode [pre-send-rd, wr). Send/wait/ACK logic
  untouched. Our tree has **no** `rpmh_votes.cnoc/ddr` structs (hardcoded
  msg) — stated, not retrofitted.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- SDE/display 0; DT = 121 content; production untouched.
- ramdisk `eb5d2eb5…`; image 62 `e19c71fa…` untouched.
- Image 124 `124-halium-boot-a6xx-bwforensic.img`
  sha256 `241bdcb0fd2c5889cd985f6c20917e21426d0c8140550773d39598680f228440`.

No flash performed; stopping.
