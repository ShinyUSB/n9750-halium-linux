# Image 142 (build-only) — retry + selftest×10 + vitals + post-init tripwires

## Diff (all gated or logging-only)
- Resume: bounded retry (3 attempts: normal path, then `a6xx_gmu_reset`
  teardown+reboot for firmware lottery; success path byte-identical).
- Selftest: 10-iteration `cp_init` loop with per-iter RPTR/WPTR/STATUS
  (existing flags).
- Vitals: 30s delayed-work poller (STATUS/SPTPRAC/RPMHPWR/OOB/RPTR/WPTR/
  fence) armed only on nonlegacy hw_init success.
- Tripwires: hangcheck entry/lockup, recover_worker entry, first
  userspace submit (nr_cmds/nr_bos/valid).
- Untouched: all proven paths (vote/DCVS/GBIF/ADDR64/SECVID-skip).

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- SDE/display 0; DT = 121 content; production untouched.
- Packaged all-flags incl. corrected `a6xx.a6xx_selftest` (a6xx-first
  cmdline order retained against tail truncation).
- Image 142 `142-halium-boot-a6xx-stable142.img`
  sha256 `a04716f1cd3a88040479056ca2e17e5a06161d6add0f8da9c9d77e094ef2b34d`.

No flash performed; stopping.
