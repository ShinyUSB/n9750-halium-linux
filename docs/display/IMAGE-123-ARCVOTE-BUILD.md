# Image 123 (build-only) — OPP-derived ARC levels

## Exact source diff (`a6xx_gmu.c` only)
- `a6xx_gmu_get_arc_level()`: for `freq != 0`, return
  `dev_pm_opp_get_voltage(exact OPP)` instead of reading legacy
  `qcom,level` (absent everywhere). `freq == 0` still returns 0;
  `dev_pm_opp_put()` retained; error OPP still returns 0 (logged at
  runtime via ARC123/vote lines, no fallback added).
- **4.14 adaptation (reported, not hidden):** the prescribed
  `dev_pm_opp_get_level()/opp->level` does not exist in this 4.14 tree
  (no `level` field, no `opp-level` parsing in `opp/of.c`). The
  downstream DT convention stores the RPMh ARC corner in `opp-microvolt`,
  so `get_voltage()` returns exactly the OPP-attached level. No DT
  property added, no value invented or hard-coded.
- Diagnostics: `ARC123: dev/freq/level/vote` per entry in the vote loop;
  `ARC123: cmd_db counts` in `rpmh_votes_init`. Call devices unchanged
  (GX→GPU pdev, CX→GMU pdev). `a6xx_opp_node_by_freq` kept
  `__maybe_unused`.
- Untouched: BW/HFI/ACK/firmware/DT/OOB/DCVS/power/MMU/hw_init.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- SDE/display 0; DT = 121 content (36-line diff count unchanged);
  production untouched; image 62 `e19c71fa…` untouched.
- Image 123 `123-halium-boot-a6xx-arcvote.img`
  sha256 `d08be177852aecf4d60113694d616c1694f6fd18621b3128d3d50dd9dd05e568`.

No flash performed; stopping.
