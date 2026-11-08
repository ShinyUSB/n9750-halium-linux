# Image 122 — OPP fixed; PERF carries 7 real levels, ACKs; BW still timeout

## Ownership
- `GMU_DEV: before==after=2c6a000.qcom,gmu (/soc/qcom,gmu@0x2C6A000)`,
  `cb=2c6a000.qcom,gmu:gmu_kernel`. `gmu->dev` never leaves the GMU pdev.

## OPP (all criteria met)
- `opp_probe_ret=0` (was -ENODEV). `rpmh_votes_init ret=0` (executed).
- `nr_gmu_freqs=4`: `0, 19200000, 200000000, 500000000`
  (live DTB carries extra 19.2M/500M levels beyond the single DT entry).
- `nr_gpu_freqs=7`: `0, 257M, 345M, 427M, 499.2M, 585M, 675M`
  (GPU OPP independent issue: none — present and parsed).

## PERF payload (PERF120)
- `header=0x00203b04 total_dwords=59 gpu_level_num=7 gmu_level_num=2`.
- GX[0..6]: real freqs above, `vote=0x0` all, `acd=0xffffffff` all.
  (Votes still zero: `get_arc_level` reads absent `qcom,level` → 0.
  Recorded as the next anomaly, not fixed here.)
- `CX[0]={0,0} CX[1]={0,200000}` (forced shape, existing votes).

## Bootstrap
- `FW_VERSION -> ACK (id=1)`. `PERF -> ACK (id=4, ret=0)`.
- `BW (canonical 110) -> TIMEOUT (-110)`. First subsequent failure: BW.
- `hw_init` suppressed, device alive. SMMU faults/panics: 0.
- HFI/GMU IRQ counts: no counters in image (2 HFI answers observed).

Image 122 `4332bb566ecf557603a4fa65e4c55d92bc44a3c05049817643b5499fb227207a`.
SDE 0, DT = 121 content, production untouched. Stopping.
