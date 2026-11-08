# Image 120 (build-only) — fixed CX shape (gmu_level_num=2)

## Exact diff (`a6xx_hfi.c`, `a6xx_hfi_send_perf_table` only)
- `num_gmu_levels`: was `gmu->nr_gmu_freqs` (runtime OPP count+1) → fixed `2`.
- CX: was loop over `nr_gmu_freqs` with `freq=gmu_freqs[i]/1000` →
  `CX[0]={cx_arc_votes[0], 0}`, `CX[1]={cx_arc_votes[1], 200000}`
  (votes from existing arrays, zero other entries).
- GX votes/freqs untouched; `acd`: was implicit `0x0` → `0xffffffff`
  (established Samsung KGSL + donor-master sentinel; no platform source
  exists per Image 119 audit).
- Added `PERF120:` prints (per-GX vote/acd/freq, CX pair + old
  `nr_gmu_freqs`, header/total_dwords/levels/ret after send fills header).
- Untouched: header format, struct size, queues, ACK dispatch, HFI IRQs,
  BW table, ECP, firmware, MMU, clocks/power, OOB/DCVS, hw_init.

## Old vs new (runtime values at boot via PERF120 log)
- old: `gmu_level_num = nr_gmu_freqs` (OPP-derived, logged in print),
  CX freqs from `gmu_freqs[]/1000`, acd=0.
- new: `gmu_level_num = 2`, `CX[0].freq = 0`, `CX[1].freq = 200000`,
  acd=`0xffffffff`. GX entries identical apart from acd.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- SDE/display 0; DT pre-existing 14 only; production untouched.
- ramdisk `eb5d2eb5…`; image 62 `e19c71fa…` untouched.
- Image 120 `120-halium-boot-a6xx-perfcx.img`
  sha256 `1f0c350427eb93dcee76dcfec71161bf703895f9d37f095d760937c4249c5358`.

No flash performed; stopping.
