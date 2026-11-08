# Image 164 (build-only) — shot ladder + all-IRQ counts

## Why
163's first success crash reframed the hunt: ticks perfect at +50ms
(+15 ticks/50ms), then death before +2s with NO Linux bark (yet TZ
says NON_SECURE_WDT) — a fast silent class distinct from the
bark-at-11s class. Need state between +50ms and +2s, plus storm
coverage on ALL THREE GPU IRQs (gpu 487 was 0; HFI/GMU lines never
logged post-run).

## Diff (on top of 163, `a6xx_gpu.c` only)
- Shot ladder: sync SHOTN1, +50ms SHOT0, +250ms SHOT1, +1s SHOT2,
  +5s SHOT3 (regs + cpubt + show_state), +30s SHOT4.
- Every shot logs `hfi_irq/hficount` + `gmu_irq/gmucount` alongside
  the existing gpu irqcount (struct fields already in-tree).
- Untouched: all 160–163 behavior.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- Binary contains `SHOT3`, `hficount=`, `gmucount=`.
- Packaged with 160 cmdline.
- Image 164 `164-halium-boot-a6xx-ladder.img`
  sha256 `f9eefeeb108744b48c926cb4815e289213647952ed6703d1db6a683ba8dc4909`.

## Readout
- Counts explode on any line between shots → storm line identified
  (fix: handler/affinity/mask).
- All quiet + regs collapse → firmware collapse (fix: slumber).
- Nothing past a given shot → death bracketed to the gap.

No flash performed (needs Download mode); stopping.
