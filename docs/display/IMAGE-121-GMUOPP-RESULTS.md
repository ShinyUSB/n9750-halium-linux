# Image 121 — outcome E (OPP mechanism found) + firmware NACKs degenerate PERF

## OPP probe (E)
- `OPP121: GMU OPP probe ret=-19` (-ENODEV). DT verified live on-device:
  `operating-points-v2` present on the GMU node, `gmu-opp-table/
  opp-200000000/opp-hz` present.
- Mechanism (code inspection): `a6xx_gmu_memory_probe` (line 2474) sets
  `gmu->dev = cb_dev` **before** `a6xx_gmu_pwrlevels_probe` (line 2627),
  so `dev_pm_opp_of_add_table(gmu->dev)` queries the `gmu_kernel`
  context-bank node (`compatible`, `iommus` only — confirmed on-device),
  which has no OPP property → -ENODEV via the v1 fallback. The DT
  content was never the problem; the device pointer was. No workaround
  added (out of scope for this image).
- Consequence unchanged: `nr_gmu_freqs=0`, `nr_gpu_freqs=0`,
  `rpmh_votes_init` never runs.

## Bootstrap (new firmware behavior on identical bytes)
- `FW_VERSION -> ACK (id=1, error=0)`.
- PERF (byte-identical to image 120: `gpu_level_num=0, gmu_level_num=2`,
  `acd=0xffffffff`, zero votes) → **firmware NACK**:
  `HFI RX: header=0x00110300 id=0 type=1 size=3 seq=1`,
  words `00110300 0da43e00 0000006f` (error=111=`0x6f` echoed).
  `send_perftbl ret=-110` (waiter 5s timeout; error reply unmatched).
- Stopped at first failure: no BW/ECP/core attempted. `hw_init`
  suppressed, device alive (uptime 166s).

## Reading
Images 118/120 (same degenerate PERF) got clean ACKs; 121 got an
explicit NACK. Response to `gpu_level_num=0` input is firmware-side
nondeterministic — consistent with invalid/degenerate input rather
than a transport issue. Fixing the real input (nonzero levels) requires
the OPP device-pointer fix, not another DT content change.

## Remaining items
- SMMU faults: 0. Panic/Oops: 0. IRQ counts: no counters in image
  (2 HFI RX packets observed: version ACK + PERF NACK).
- DCVS deferrals: 0.

Image 121 `a32b77bcff419086ebb554d6d71dfd92124ba0efa656b92351b5f8ce036e1585`.
SDE 0, DT +22/-0 (121 only), production untouched. Stopping.
