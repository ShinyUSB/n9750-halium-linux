# Image 183 — retry path must not touch the TCM aperture (NOC fix)

## Why (image 182's first trigger, fatal in 5 seconds)

182's async trigger worked as designed (write returned instantly, SSH
stayed up), but the bring-up died on the *retry*, not the wedge:

- Attempt 0: GMU starts (`BABEFACE GOOD`), firmware silent →
  `HFI-E fw_version timeout ... ret=-110` (the known lottery).
- `RETRY142: attempt 1/2` → `a6xx_gmu_reset()` tears down, cycles
  clocks, re-enters `a6xx_gmu_fw_start(GMU_RESET)` → all control-reg
  writes/reads succeed (CP_MSG, CM3_CFG, ICACHE/DCACHE, `FORENSIC REGS
  PRE`) → then `Synchronous External Abort (0x96000010) at
  0xffffff801fdc3000`, `pc: msm_readl+0x44`, `a6xx_gmu_fw_start+0xc78`
  ← `a6xx_gmu_reset` ← `a6xx_pm_resume` ← runtime-PM ←
  `adreno_load_gpu` ← `load_gpu_thread`. TZ: `TZBSP_ERR_FATAL_NOC_ERROR`.

Decoded end to end (objdump + xml):

- Fault address = `kgsl_gmu_reg` base + 0x3000 = register offset
  0xC00 = `REG_A6XX_GMU_CM3_ITCM_START`.
- `fw_start+0xc78` disassembles to a 256-iteration loop reading
  `MMIO+0x3000` into RAM — `f118_store_pre()` (`F118_ITCM_A_N`),
  inlined via `a6xx_gmu_start()` (which `fw_start` calls).
- So: control regs (always-on AHB) survive a clock disable/re-enable
  cycle; the ITCM/DTCM RAM aperture does not. The RAMs sit behind a
  switch the reset path does not restore, and the first aperture read
  after the cycle aborts the bus hard enough for TZ to reset the SoC.
  Attempt 0 (cold power-on) reads the same aperture fine, which is why
  181 never saw this: it won the lottery twice and never retried.

## Diff (one gate, on top of 182)

- `a6xx_gmu.c`, `a6xx_gmu_start()`: `f118_store_pre()` (256+72+96
  ITCM + 264 DTCM aperture words) runs only under `a6xx_instr`.
  `f118_snap_regs(PRE)` (control regs, proven safe on the fatal
  retry's own log) stays unconditional. Post-boot aperture reads
  (`f118_verdict` on GOOD/BAD, firmware alive) are unaffected.
- The PRE store was pure forensics (pre/post diff input for a verdict
  era that is over). Nothing in the functional path consumed it.

Untouched: everything else in 182 (async trigger, quiet suspend,
slumber-off default, 181 bark-pet/enable=0/no-IPI/no-escalation,
softdog+FPUT off). Cmdline byte-identical to 181/182.

## Checks

- build: 0 errors, no new warnings in adreno/gmu/msm_drv.
- `.config`: softdog/FPUT/panic-on-oops/bite-on-panic all unset (same
  as 182).
- `Image-dtb`: `BARK181` kept, `handed to a6xx-load thread` kept,
  `SSTEP171: pm_suspend enter` still absent.
- packaged with 180/181/182's ramdisk (sha `eb5d2eb5…`), same
  board/header/cmdline.

Image 183 `183-halium-boot-a6xx-notcm.img`
sha256 `b482c2684a30cdc624337430427adc39ee6a7eba201cc2865ef4457d87dd00f8`.

## Readout

- Retry after `-110` completes (attempt 1/2 runs to verdict instead of
  aborting) → the abort class is closed; the lottery becomes what it
  always should have been (a bounded wait, not a reset). Then Mesa per
  the 182 readout.
- Abort recurs elsewhere → a second aperture reader exists on a
  pre-boot path; the oops names it exactly like this one did
  (pc + fault address → xml offset → caller).
- Attempt 0 keeps winning outright → retry stays untested but armed;
  fine, the gate costs nothing when unused.

OPEN (power-sequencing debt, after Mesa): why the TCM aperture dies
across a clock disable/re-enable cycle while the control regs live.
Until root-caused, no pre-boot path may touch it.

No flash performed (needs Download mode); stopping.