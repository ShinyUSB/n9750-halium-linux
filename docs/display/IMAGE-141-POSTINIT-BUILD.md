# Image 141 (build-only) — post-init forensics + selftest flag

## Diff (logging + pre-existing selftest path)
- `msm_gpu.c`: `HANGCHK141` entry/lockup lines, recover_worker entry.
- `adreno_gpu.c`: `SUBMIT141` first-userspace-submit tripwire.
- Selftest: 137's tail block intact (`experimental_nonlegacy &&
  a6xx_selftest` → re-run `cp_init`, timeout-bounded); fixed the
  duplicated param/decl left by 137 (single registration verified).
- No devfreq target exists in-tree (dormant — noted, not added).
- Untouched: everything proven (vote/DCVS/GBIF/ADDR64/SECVID-skip).

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors. SDE 0, DT = 121.
- Packaged all-flags (probe/secure/nonlegacy/selftest):
  `141-halium-boot-a6xx-postinit.img`
  sha256 `09aa9667dc256ad908267ea04a58f1cbb7d4b1276366e4d2445a237b7af9990a`.
- Capture filter v5 staged (adds POSTINIT/SELFTEST/HANGCHK/SUBMIT141/
  BC132/OOB132/NONLEGACY/RACE markers).

## Run plan
Flash → deploy v5 filter → trigger → expect SELFTEST137 (first live
workload proof) → watch post-init (hangcheck/recover/submit/WDT).
Also: pristine-idle causality watch still open.

No flash performed; stopping.
