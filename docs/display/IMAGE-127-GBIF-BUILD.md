# Image 127 (build-only) — A640 GBIF path, VBIF excluded

## Exact diff
- `a6xx.xml.h`: 5 new `REG_A6XX_GBIF_QSB_SIDE0-3` (`0x3c03-06`) +
  `REG_A6XX_RBBM_GBIF_CLIENT_QOS_CNTL` (`0x11`); offsets from Samsung
  vendor `a6xx_reg.h`, dword convention of the file.
- `a6xx_gpu.c` hw_init: `adreno_is_a640()` selects the vendor-verified
  `a640_gbif` table (SIDE0-3 `0x00071620`, QOS `0x3`) with `GBIF[00..04]`
  + readback markers; else-branch preserves the A630 VBIF pair.
  VBIF writes excluded for A640 (not merely preceded).
- Untouched: HFI/OPP/PERF/BW/ECP/firmware/MMU/clocks/power/OOB/DCVS/DT
  and all other hw_init stages. Fixes 122/123/125 retained.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- SDE/display 0; DT = 121 content; production untouched.
- ramdisk `eb5d2eb5…`; image 62 `e19c71fa…` untouched.
- Image 127 `127-halium-boot-a6xx-gbif.img`
  sha256 `374e64ee470254eec66b9ef9ca2237ae0b98c91ba100b112346254e9bfdf94e3`.

No flash performed; stopping.
