# Image 162 (build-only) — death-watch round 2

## Why
161 proved death <200ms post-`[25]` (armed, no SHOT0) but left no
state. Two suspects fit a sub-200ms window: an IRQ storm the instant
`enable_irq(gpu->irq)` runs after hw_init, or autonomous firmware
collapse right after the [23]/[24] OOB handoff. DT audit: SPI 300 is
the standard GPU IRQ across sdm660/sm8150/sm6150 trees, no conflict —
storm needs live proof, not DT proof.

## Diff (on top of 161, `a6xx_gpu.c` only)
- SHOTN1: synchronous reg dump at arm time (always lands, runs before
  the caller's `enable_irq`).
- First delayed shot shortened 200ms → 50ms.
- Every shot now logs `gpu_irq=<n> irqcount=<kstat_irqs>`:
  irqcount delta between SHOTN1 (pre-enable) and SHOT0 (+50ms,
  post-enable) PROVES or EXONERATES the storm with numbers.
- Untouched: all 160/161 behavior.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- Binary contains `DW161: armed (sync`, `SHOTN1`, `gpu_irq=`.
- Packaged with 160 cmdline.
- Image 162 `162-halium-boot-a6xx-watch2.img`
  sha256 `191c16c81dec0be64ab7c4bdad30a9284f1c9deff3b0db6045e61437bde30995`.

## Readout
- irqcount explodes SHOTN1→SHOT0 → IRQ storm at enable_irq (fix: mask
  handling / affinity / handler audit).
- GX regs collapsed at SHOT0 with quiet irqcount → firmware collapse
  (fix: slumber handshake / keep-awake).
- Nothing past SHOTN1 → death inside `[25]` return path itself.

No flash performed (needs Download mode); stopping.
