# Image 120 — outcome B: CX-shape hypothesis ruled out

## Result
```
FW_VERSION -> ACK (id=1)
PERF_TABLE (fixed CX shape) -> ACK (id=4, ret=0)
BW_TABLE (canonical 110) -> TIMEOUT (-110)
BOOTSTRAP END; hfi ret=-110; hw_init suppressed, device alive
```
Identical to Image 118/110 with old CX shape. The fixed CX shape changes
nothing → this hypothesis is ruled out. No BW change made.

## Actual runtime payload (PERF120 log — first-ever dump)
```
header=0x00203b04 total_dwords=59 gpu_level_num=0 gmu_level_num=2
GX lines: none (nr_gpu_freqs=0)
CX[0] vote=0x00000000 freq=0 | CX[1] vote=0x00000000 freq=200000
(old nr_gmu_freqs=0)
```

## Root cause of the zeros (code inspection, same boot log)
`a6xx_gmu_pwrlevels_probe` (`a6xx_gmu.c:2077`): `dev_pm_opp_of_add_table`
fails (`Unable to set the OPP table for the GMU`, present every boot) →
early return → `nr_gmu_freqs`/`nr_gpu_freqs` stay 0, `rpmh_votes_init`
never runs, all votes/freqs zero. So **every** PERF sent to date
(110/117/118/120) carried `gpu_level_num=0` — yet firmware ACKs it both
with `gmu_level_num=0` (118) and `=2` (120). Recorded, not fixed here.

## Remaining report items
- HFI/GMU IRQ counts: no counters in this image (ACK lines show 2 answers:
  id=1, id=4; no GMU-IRQ/firehose anomalies in stream).
- SMMU faults: 0. Panics/Oops: 0. Device alive at uptime 145s.
- First subsequent failure: BW_TABLE timeout, unchanged.
- DCVS deferrals: 0 (gate present, unused — still no async caller).

Image 120 `1f0c350427eb93dcee76dcfec71161bf703895f9d37f095d760937c4249c5358`.
SDE 0, DT +0, production untouched. Stopping.
