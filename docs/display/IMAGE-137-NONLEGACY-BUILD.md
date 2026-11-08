# Image 137 (build-only) — gated nonlegacy mode + post-init trace

## Source changes (all cmdline-gated, default off = identical behavior)
- Params (all under-prefix verified): `a6xx.experimental_nonlegacy`,
  `a6xx.selftest` (+ existing probe/secure).
- Resume: vote if `probe||nonlegacy`; boot `set_freq` skipped under
  nonlegacy (`NONLEGACY137` log); SECVID skip if `secure==1||nonlegacy`.
- `msm_gpu.c`: `bc132()` last-32 ring (printk-persisted via last_kmsg) +
  `POSTINIT[00/10/20/30/40]` + runtime_resume/suspend logs.
- `a6xx_gmu.c`: `OOB132` set/clear lines.
- `a6xx_gpu.c` tail: `SELFTEST137` (re-run `cp_init`, timeout-bounded).
- Part 8: still zero `a640_zap` blobs (host stock + device vendor/fw).

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- SDE/display 0; DT = 121 content; production untouched.
- Default cmdline (all flags off) packaged:
  `137-halium-boot-a6xx-nonlegacy.img`
  sha256 `668f966cfe92241693153de118194a5ec54fdd16640a538fc2dcc5f00be4b07e`.
- Test cmdlines (A/B/C) = repackages of this kernel, no rebuild.

No flash performed; stopping (TEST A first after flash).
