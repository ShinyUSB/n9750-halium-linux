# Image 138 (build-only) — boot-race forensics, no functional change

## Diff (instrumentation only)
- `a6xx_gmu.c`: `ATT138` per-attach identity (n, pdev/ptr, OF node,
  parent, mmio/domain state, `%ps` caller) + `PROV138` provider snapshot
  (PM enable/active/usage, cx/gx regulators, domain/cb, clk rates,
  RPMH_CTRL; notes: no-gxpd/devfreq-target/qmp, links skipped) at probe
  entry/end; `BOOT_RACE_READY` (ms + uptime) at probe completion;
  `bc132` in resume entry/return; `PLUS10MS` vote snapshot.
- `msm_gpu.c`: `BC132_N` 32→64 (printk-persisted via last_kmsg).
- Duplicate mechanism (source): `load_gpu_real` attaches only if
  `priv->gpu==NULL`; destroy-on-failure re-arms it. Only sysfs writes
  attach; no on-device writer found; Weston open = held, no attach.
- Part 7 delay param: NOT added (SSH floor ~25s makes T+0/5/15
  unreachable by hand; timing via on-device scheduled triggers instead).

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- SDE/display 0; DT = 121 content; production untouched.
- Packaged probe=1 + secure=1 (max observability, proven path).
- Image 138 `138-halium-boot-a6xx-bootrace.img`
  sha256 `6fb46f50f420bf8b7a5b276394f0edfe51f0df146e044b9cf8f5b23ded09a828`.

No flash performed; stopping (timing runs after flash).
