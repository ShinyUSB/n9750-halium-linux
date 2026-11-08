# Image 96 — GMU timeout state capture (TWO full runs, no SMMU fault)

## Setup that worked

`a6xx_gmu_boot_error` flag + `msm_gpu_hw_init` gate: on `-ETIMEDOUT` the
attach stops with `A6XX_DIAG: skip hw_init` and the system stays alive.
Two triggers in one boot (38 s, 179 s) both ran the complete sequence.

## A/B/C/D register state (identical across both runs)

| point | SYSRESET | INITRES | FWBUSY | NMI | WFI | BOOTCFG |
|---|---|---|---|---|---|---|
| A pre-release | 0x1 | 0x0 | 0x0 | 0x783cfef | 0x0 | 0x2 |
| A asserted | 0x1 | 0x0 | 0x0 | 0x783cfef | 0x0 | 0x2 |
| B post-release | 0x0 | 0x0 | 0x0 | 0x783cfef | 0x0 | 0x2 |
| C poll-enter | 0x0 | 0x0 | 0x0 | **0x0** | 0x0 | 0x2 |
| D timeout | 0x0 | **0xbaddface** | 0x0 | 0x0 | 0x0 | 0x2 |

QTBL `0x60001000` / INFO 1 / SFR `0x6040100` / G7 1 / HFISTAT 0 stable
throughout. No SMMU fault in either run.

## Seven answers

1. **Timeout state:** table above; GMU out of reset, clocks on, idle.
2. **FW_START asserted:** no such register exists in either tree; the
   `fw_start()` function completed through `power_config`; GENERAL_7 = 1.
3. **FW_INIT completed:** yes, with failure verdict (see 4), FWBUSY = 0.
4. **INITRES at timeout: `0xbaddface`** — written by firmware between C and D
   (was 0x0 at C). Deliberate failed-boot verdict (vs `0xbabeface` success).
5. **Changed FW_START→fault window:** NMI `0x783cfef`→`0x0`, INITRES
   `0x0`→`0xbaddface`, SYSRESET `0x1`→`0x0` (our release). All else stable.
6. **Who zeroed scratch:** NOT isolated to one op. Zeros present by
   post-clocks, unchanged through timeout. Window remains probe→clock-enable
   (MMIO, IRQ, pwrlevels, hfi_init, fw file reads, CX, clk_set_rate). GMU in
   reset cannot DMA, so either unreset-clocked execution or an unidentified
   host op. Left open.
7. **KGSL-vs-DRM divergence at timeout:** only the SYSRESET assert pulse
   (ours 1→0, KGSL 0-only) and the absent CP_MSG/log. All programmed values
   match.

## Notes

- The SMMU fault did NOT reproduce in either run (same binary that faulted on
  image 95). Difference from faulting runs: capture MMIO reads in the start
  window (timing perturbation) and/or the sentinel mapping now present from
  boot. Not concluded; recorded.
- Long boot ended without panic (last_kmsg stops at 179.28 s, no fault/oops);
  reboot cause unknown, unrelated to GPU (gate held, system idle).
- Image 96 `9fb381143ff8f9e6bafe8d9cac20d2e646ec78b4f012f0e3d4ffaa75db3caad1`.
  SDE 0, DT +0, production untouched. Diagnostic instrumentation only.