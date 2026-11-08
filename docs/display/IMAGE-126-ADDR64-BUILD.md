# Image 126 (build-only) — address-mode region traced, markers only

## Premise correction (verified tree-wide)
The 12-register 64-bit sequence does NOT exist in our driver: zero
`ADDR_MODE_CNTL` writes in any `.c` file (defines only in `a6xx.xml.h`).
Per the no-fabrication rule, no per-register ADDR64 before/after markers
were added (nothing to bracket) and no writes/values were introduced.
Instead the actual neighbors are instrumented; the absence itself is the
prime suspect for a future functional image.

## Marker diff (`a6xx_gpu.c` only)
- Kept `[07]`; added `ADDR64[00..11] ABSENT` line (all 12 offsets recorded).
- `[08]` after the empty region + `RBBM_STATUS`/`STATUS3` snapshot.
- `[09]`/`[10]` around `a6xx_set_hwcg()` + STATUS snapshots.
- `[11]`/`[12]` around the VBIF-start block (our actual next stage;
  GBIF/QSB as named does not exist) + STATUS snapshot.
- Read-only `gpu_read()` snapshots only; no writes added/reordered.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- SDE/display 0; DT = 121 content; production untouched.
- ramdisk `eb5d2eb5…`; image 62 `e19c71fa…` untouched.
- Image 126 `126-halium-boot-a6xx-addr64trace.img`
  sha256 `a8dfd305807038e6fc54fca1a637fbfff4760fba316336210f69416ba40cfd78`.

No flash performed; stopping.
