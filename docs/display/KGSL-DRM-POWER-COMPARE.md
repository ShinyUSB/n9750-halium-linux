# KGSL vs DRM power/resource sequence + reset-reason evidence

## Reset reason: ANSWERED — kernel panic via SMMU fault, not PMIC/brownout

Samsung `restart_reason` after our trigger boots: **`0x776655ee` = KERNEL PANIC**
(`collect_rr_data : restart_reason = 0x776655ee`, `upload_cause = KERNEL PANIC`).

The panic path is Samsung `sec_debug` (writes `/proc/last_kmsg`, sets dload
magic) — which is why `pstore`/ramoops is always empty. `/proc/last_kmsg`
(HOLD the previous boot) contains the full crash. Two trigger boots captured:

| boot | last marker → fault | SMMU detail |
|---|---|---|
| uptime 391 (pid 4661) | `PRE_PDCSEQ` → +1.6 ms | `2ca0000.kgsl-smmu: Unhandled context fault: iova=0x000433c0, cb=1, fsr=0x40000402, fsynr0=0x60033` → `Kernel panic: gmu_kernel SMMU Fault` |
| uptime 121 (pid 1138) | `PRE_PDCSEQ` → +2.0 ms | `iova=0x000b54e0, cb=1, fsr=0x40000402, fsynr0=0x60033` → `Kernel panic: No Device SMMU Fault - SID=0x405` |

(`fsr=0x40000402` + `fsynr0=0x60033`: read fault (WnR=0 in SYNR0 bit 30 clear);
`cb=1`; ATOS fails — genuinely unmapped.)

So the PMIC/brownout hypothesis is **withdrawn**: the resets are SMMU-fault
panics. And the "moving silent death" is explained: host-side streams died with
SSH before the panic printed, and panics never reach pstore — only `last_kmsg`
holds them, which I only just read.

## Where the fault sits

Both boots executed identically through:

```
probe COMPLETE → fw[0] → fw[1] → resume → CX vote ret=0 → 5× prepare ret=0 →
5× enable ret=0 → IRQ enabled → cold → fw_start → PRE_RPMH (pointers unchanged) →
PRE_PDCSEQ → +1.6~2.0 ms → SMMU read fault on kgsl-smmu cb=1
```

Fault IOVAs (`0x433c0`, `0xb54e0`) are NOT any region we mapped (icache
0x4000–0x3FFFF, dcache 0x44000+, hfi dynamic). Something — GMU hardware or a
stale pointer — reads nearby-but-wrong addresses ~2 ms after rpmh_init starts.
Note `0xb54e0` vs our `MMIO_MAP`-era values: the two boots fault at DIFFERENT
iovas, so it is not one fixed wrong address.

## KGSL power/resource sequence (this device, A640)

`gmu_start()` (INIT/SUSPEND/SLUMBER/RESET identical prefix):

```
1. gmu_aop_send_acd_state()          AOP/QMP ACD mailbox
2. gmu_enable_gdsc()                  regulator_enable(cx_gdsc)  [vddcx ONLY]
3. gmu_enable_clks()                  clk_prepare_enable per clock, NULL-terminated
4. irq_enable()
5. msm_bus_scale_client_update_request(gmu->pcl, 300MHz)   [INIT only]
6. rpmh_gpu_pwrctrl(FW_START, COLD)   → GENERAL_7=1 → rpmh_ucode → load_gmu_fw →
   CM3 regs → power_config → gmu_start → gfx_rail_on → sptprac → hfi_start
7. kgsl_pwrctrl_set_default_gpu_pwrlevel(); bus vote → 0
```

Answers to the five questions:

1. **GX regulator vote at boot? NO — not via regulator.** `gx_gdsc` is only
   touched in the SUSPEND path (enable/disable dance). At boot, GX comes up via
   `a6xx_gmu_gfx_rail_on()`: GMU-register writes (`GX_VOTE_IDX`, `MX_VOTE_IDX`)
   + `oob_boot_slumber` — i.e. **the GMU itself votes GX through RPMh**, exactly
   as the "GMU is the only one controlling the GX HS" comment states.
2. **Before/after clocks?** N/A — no host-side GX vote exists at boot.
3. **Quirk-conditional?** CX handling (`regulator_set_mode IDLE/NORMAL` in
   suspend; GDSC polling) is gated on `ADRENO_QUIRK_CX_GDSC` ← DT
   `qcom,gpu-quirk-cx-gdsc`, **which our DT sets** but our DRM never parses
   (zero `gpu-quirk` handling in `adreno_device.c`).
4. **GMU vs host owns GX? GMU**, via RPMh (our `rpmh_init` already sets
   `RPMH_CTRL_GFX_VOTE_ENABLE`).
5. **DDR/BW vote before fw/device start? YES** — 300 MHz via `gmu->pcl` before
   `rpmh_gpu_pwrctrl(FW_START)`, released after `hfi_start`.

## DRM sequence (ours)

```
pm_runtime_get_sync (no pm_ops → no-op)
→ CX enable (added, ret=0)      [KGSL step 2 equivalent]
→ clk_set_rate + 5× prepare + 5× enable
→ GMU IRQ enable                 [KGSL step 4]
→ GENERAL_7=1 → rpmh_init (PDC writes, FIXED) → fw_load → CM3/HFI_QTBL regs →
  power_config → gmu_start → gfx_rail_on → sptprac → hfi_start
```

## Exact difference

| # | KGSL | DRM | status |
|---|---|---|---|
| 1 | AOP/ACD mailbox first | **missing** | open |
| 2 | CX GDSC enable | **done** (ret=0) | closed |
| 3 | clocks (NULL-terminated) | done (counted loop, fixed alloc) | closed |
| 4 | IRQ enable | done | closed |
| 5 | **DDR 300 MHz bus vote before FW_START** | **missing** | open — prime suspect #1 |
| 6 | rpmh FW_START → ... → HFI | same shape | under test |
| 7 | GX via GMU RPMh/OOB | same shape (`gfx_rail_on` exists) | under test |

Neither quirk parsing (`cx-gdsc` set in DT, ignored) nor ACD nor the bus vote
exists in our tree.

## Next single vote

The DDR/CNOC bus vote before `FW_START` is the only KGSL step in the
clocks→firmware window that we lack. It is also mechanically plausible for an
SMMU fault: without the interconnect vote, GMU DMA (firmware/HFI reads issued
right after `gmu_start`) traverses an un-voted CNOC path. Recommend adding
**only** that vote next — not ACD, not GX, not quirk parsing.