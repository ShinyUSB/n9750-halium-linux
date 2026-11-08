# Image 168 — stay-awake render window (repackage, no rebuild)

## Why
167 proved the userspace path (`FD640`, ES 3.1, configs) on a
suppressed boot. Real pixels need a POWERED GPU: on success boots 167
slumbers synchronously, so userspace always finds it asleep (and no
resume-on-submit exists yet). For the first render + CP IRQ
verification, take the same 167 kernel with slumber disabled via
cmdline — success runs stay awake in their (variable) survival
window, during which a ~2s glClear+readback + fence wait either
completes (render + IRQ proven) or the boot dies trying.

## Diff
- None in source (167 kernel bit-identical). Cmdline adds
  `a6xx.slumber165=0` (the documented off switch).
- Image 168 `168-halium-boot-a6xx-awake.img`
  sha256 `cc92e3b907b2fe6bdcee9939d3b7d27724c546200cd54cea8e648e9bb2945e11`.
  (Rev 2: a6xx flags moved to cmdline front — the bootloader truncates
  ~512B and the appended `slumber165=0` never arrived in rev 1.)

## Readout
- `RENDER-OK` + pixel red → first real GPU workload; CP IRQ/fence
  path proven live. Then: resume-on-submit design toward Mesa proper.
- Submit wedges (glFinish never returns) → CP IRQ path broken;
  diagnose via RBBM INT status + retire path, fix, retest.

No flash performed (needs Download mode); stopping.
