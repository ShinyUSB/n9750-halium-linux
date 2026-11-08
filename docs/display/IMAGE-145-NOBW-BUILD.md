# Image 145 (build-only) — gated BW skip (PERF-sent + BW-skipped cell)

## Diff
- `a6xx.skip_bw` param (placement verified UNDER prefix) + `skip_bw`
  gate in v2 `hfi_start`: log `BW145: SKIPPED`, `ret=0`, continue to ECP.
  Canonical table object untouched. All 144 behavior otherwise identical.

## Why this cell
- 113 (zero-vote PERF + BW skip) → ECP timeout. Untested: real-vote
  PERF + BW skip → does ECP answer? If yes: no-BW path further; if no:
  BW state required for chain (as 113 suggested).
- Survival correlation: BW-sent runs die; 113 (BW never sent, no hw_init)
  stayed stable. But 115/128 rebooted without BW effects — ambiguous.
  This run discriminates the BW-side-effects theory one variable at a time.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- SDE/display 0 (verify at flash); DT/prod untouched (no DT/src contact
  outside the two adreno files).
- Packaged a6xx-first cmdline (probe/secure/nonlegacy/selftest/skip_bw).
- Image 145 `145-halium-boot-a6xx-nobw.img`
  sha256 `f2f6040f3add7767c6e5017b3f74daa37369f13ec641f474f77709ee5d9685f2`.

No flash performed; stopping.
