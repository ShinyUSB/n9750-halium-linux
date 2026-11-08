# Image 148 (build-only) — GX force-disable guard

## The one change (`a6xx_gmu.c`, reset path)
Skip `regulator_force_disable(gmu->gx)` unless the regulator exists and
reports enabled (`GXGUARD148` log line either way). Our resume enables CX
only; force-disabling an untouched GX is an unbalanced-disable BUG.

## Checks
- full: EXIT 0, 0 errors. SDE/display 0; DT = 121 content; prod untouched.
- Packaged canonical path (probe/secure/nonlegacy/selftest, NO skip_bw).
- Image 148 `148-halium-boot-a6xx-gxguard.img`
  sha256 `fa7b670b740b1460dee67acb9c6df0233e3fb23ba0508ad685af2ca5f06a429a`.

No flash performed; stopping (retry gets first real test after flash).
