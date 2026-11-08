# Image 155 (build-only) — GPU_SET skip test

## Theory under test
Deaths ⟺ hw_init ENTERED holds across all runs (113/suppress/pristine
live; everything else dies). Within hw_init, the GPU_SET OOB is the
firmware state-advance signal; hypothesis: firmware's post-GPU_SET
expectations (contact/slumber/votes it never gets) kill the system
minutes later. Skipping GPU_SET tests exactly that.

## Diff
- `a6xx.skip_gpuset` param (UNDER-prefix verified) + gate around the
  single `set_oob(GPU_SET)` call (`SKIPSET155` log). All else identical.
- Possible outcomes: void writes/-22 + long life (power story), or
  sticky + [25] (GPU_SET unnecessary) + survival datum either way.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- Packaged all-flags + skip_gpuset=1 (hold_boot=0).
- Image 155 `155-halium-boot-a6xx-skipgpuset.img`
  sha256 `0d1fe069431512070567d849a9c710fc9e673a9d4029fe47e40bafbb98d79030`.

No flash performed; stopping.
