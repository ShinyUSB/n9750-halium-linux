# Image 125 (build-only) — HFI-v2 queue alignment parity, oversize non-fatal

## Exact diff (`a6xx_hfi.c`, `a6xx_hfi_queue_read` only)
- Signature now takes `gmu` (sole caller updated); no other callers.
- `BUG_ON(size > dwords)` → log `HFI_RX OVERSIZE` + `dev_err`, return
  `-EMSGSIZE`, index untouched (existing tasklet loop already breaks
  gracefully on `< 0`, device stays alive).
- After payload copy, if `!gmu->legacy` (this tree's HFI-v2
  discriminator; no `gmu->ver.hfi` exists): `index = ALIGN(index, SZ_4)
  % header->size` with `HFI_RX advance old/raw_end/aligned` log. v1
  untouched. Pre-read `HFI_RX index/hdr/id/type/size/seq` log added.
- 109 ACK dispatch, 108/111/112 diagnostics, PERF/BW/ECP/OPP/ARC/DT/
  firmware/MMU/clocks/power/OOB/DCVS/hw_init: all untouched.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- SDE/display 0; DT = 121 content; production untouched.
- ramdisk `eb5d2eb5…`; image 62 `e19c71fa…` untouched.
- Image 125 `125-halium-boot-a6xx-qalign.img`
  sha256 `7c55f6e27cd9454b18099fc40cd2ede8f7fd33657abe95f3bf8c5245281bc32d`.

No flash performed; stopping.
