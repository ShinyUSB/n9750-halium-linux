# Image 131 — STOP: upstream vote API does not exist in this tree (C + PART 6)

## Absence (tree-wide grep over `drivers/gpu/drm/msm/`, zero hits)
`a6xx_gmu_set_initial_freq`, `a6xx_hfi_set_freq`, `GX_BW_PERF_VOTE`,
`dev_pm_opp_set_opp`, `current_perf_index`, `gmu->freq`, `ack_type` —
**none exist**. There is no HFI frequency-vote path to instrument; no
image can be built for Parts 1–4 without fabricating the API.

## Outcome C (control-flow reason)
Our transplant predates the HFI-vote design. The actual frequency path:
1. `set_freq(highest)` = `DCVS_ACK_OPTION`/`DCVS_PERF_SETTING`/
   `DCVS_BW_SETTING` writes + `DCVS_SET` OOB, post-bootstrap in resume
   (result ignored; OOB ACK has timed out in every observed run).
2. No `INITFREQ`/`SETFREQ`/vote struct anywhere. Expected values
   (`freq=6, bw=0, ack_type=1`) have no referent here.

## PART 6: MISSING_INITIAL_GPU_OPP_SET
Confirmed — no such call in this path (or anywhere in our `drm/msm`).
Not added, per spec.

## PARTS 5/7/8 (already answered by image-130 data)
- GXPRE≈GXPOST≈FINAL_GX: `SPTPRAC=0x80 CLK_OFF=1`, votes/RPMHPWR zero at
  all four GXSTATE points — no transition exists to bracket.
- `gmu->dev` runtime: enabled=1, get_sync traced. `gxpd`: absent (130).

## Recommendation (not executed)
A future image should instrument OUR `set_freq` (DCVS setting value,
OOB ret, `DCVS_RETURN`) and confront the GMU-autonomous-power question
(GX clock only the firmware can raise after START) — different
mechanism, different image. No source touched for 131; no image built.
