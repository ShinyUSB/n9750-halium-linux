# Image 121 (build-only) — GMU OPP table in DT

## Exact DT diff (`sm8150-gpu.dtsi`, additions only, 0 removals)
- New top-level `gmu_opp_table: gmu-opp-table` (`operating-points-v2`,
  single `opp-200000000`: `opp-hz=200000000`,
  `opp-microvolt` + `opp-level` = `RPMH_REGULATOR_LEVEL_MIN_SVS`).
  Symbol verified in `include/dt-bindings/regulator/qcom,rpmh-regulator.h:24`.
  `opp-microvolt` follows in-tree convention (sdmshrike GMU table,
  `gpu_opp_table`); `opp-level` mirrors the SM8150/A640 reference.
- GMU node: `operating-points-v2 = <&gmu_opp_table>;` (+ one comment line).
- GPU OPP table/frequencies untouched. All other DT: pre-existing
  14-line `gmu` phandle only.

## Code diff (`a6xx_gmu.c` diagnostics only)
- `OPP121: GMU OPP probe ret=%d` after `dev_pm_opp_of_add_table`.
- `OPP121:` per-entry `gmu_freqs[]`/`gpu_freqs[]`, counts, and
  `rpmh_votes_init ret=%d`. PERF120 payload dump retained.

## Checks
- fast: 0 errors, 0 warnings. full (incl. DTB): EXIT 0, 0 errors.
- SDE/display 0; production untouched; image 62 `e19c71fa…` untouched.
- Image 121 `121-halium-boot-a6xx-gmuopp.img`
  sha256 `a32b77bcff419086ebb554d6d71dfd92124ba0efa656b92351b5f8ce036e1585`.

No flash performed; stopping.
