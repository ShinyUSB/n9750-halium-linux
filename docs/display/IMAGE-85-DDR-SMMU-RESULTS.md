# DDR vote + SMMU fault analysis (image 85)

## 1. DDR vote: implemented exactly per KGSL, succeeds, fault remains

KGSL source (`gmu_start()`): `msm_bus_scale_client_update_request(gmu->pcl,
pwr->pwrlevels[default].bus_freq)` — "Vote for 300MHz DDR for GMU to init" —
with `pcl` from `msm_bus_scale_register_client(msm_bus_cl_get_pdata(gpu_pdev))`
on the GPU's grp3d table (13 cases; case 4 = bus=300).

Ours (same mechanism, same table): probe registers `gmu->bw_pcl` via
`priv->gpu_pdev`; resume votes index 4 after IRQ enable, before `fw_start`.

Runtime: `before DDR vote (pcl=13)` → `after DDR vote (ret=0)`. Real client,
real success. API is live (`CONFIG_QCOM_BUS_SCALING=y`), not stubs.

**Per the decision tree: DDR succeeds but the identical translation fault
remains → DDR is NOT the (sole) root cause. IOVA/CB analysis takes priority.**

## 2. SMMU CB identity: resolved, no contradiction

`cb=%d` = `cfg->cbndx`, the arm-smmu driver's internal bitmap index — NOT
hardware CB 5. Our boot creates gfx3d first, gmu_kernel second, so gmu_kernel
plausibly IS cbndx 1. The `gmu_kernel SMMU Fault` name comes from an independent
fwspec SID match. **Do not chase CB numbering.**

DT SIDs: gfx3d_user 0x401, gfx3d_secure/gmu_user/gmu_kernel 0x400 (masks
0x0/0x2/0x4/0x5). Our runtime: gmu fwspec SID[0]=0x4000005.

Fault SIDs: one matched 0x400 (`gmu_kernel SMMU Fault`), three are **0x405**
(`No Device` — no domain claims it). SID 0x405 is a GPU-block master inside
CB1's SMR group with no attached context. All faults: `fsr=0x40000402`,
read, ATOS fails — genuinely unmapped.

## 3. Map diagnostics (runtime, before FW_START)

```
hfi    iova=0x60000000 size=0x4000 ok
icache iova=0x4000     size=0x3c000 ok
dcache iova=0x44000    size=0x3c000 ok
debug  NULL
dummy  NULL
```

Fault IOVAs across boots: `0x433c0` (icache–dcache gap), `0xb54e0`, `0x81ce0`,
`0x89ce0` (at/just past dcache end 0x80000). None inside any mapping; they move
between boots.

## 4. Firmware contract: image fully covered, runtime is not

All 7 `a640_gmu.bin` blocks vs our mappings:

| addr | size | target | covered |
|---|---|---|---|
| 0x0 | 0x30 | ITCM (reg writes) | yes |
| 0x200 | 0x140 | ITCM | yes |
| 0x400 | 0x38bc | ITCM | yes |
| 0x4000 | 0x3a58 | icache [0x4000,0x40000) | yes |
| 0x40000 | 0x1b20 | DTCM (reg writes) | yes |
| 0x43fe0 | 0x14 | DTCM | yes |
| 0x57000 | 0x308 | dcache [0x44000,0x80000) | yes |

No "failed to match fw block" prints. The static image is fully placed — the
faults are RUNTIME reads (GMU executing), not load addresses.

## 5. Prime suspect: missing `CM3_CFG` write (NMI boot)

KGSL `a6xx_gmu_fw_start()` vs ours, register by register:

| write | KGSL | DRM (ours) |
|---|---|---|
| GENERAL_7=1 | yes | yes |
| rpmh_ucode | yes | yes |
| fw image | yes | yes |
| INIT_RESULT=0 | yes | yes |
| BOOT_CONFIG | load_mode | **hardcoded 0x02** |
| QTBL_ADDR/INFO, FENCE | yes | yes |
| **CM3_CFG=0x4052** | **yes** | **MISSING (not even in our XML)** |
| SFR=chipid | yes | yes |
| CP_MSG=log_info | yes (needs gmu_log) | missing (no log buffer at all) |
| power_config → start → gfx_rail → sptprac → hfi | yes | yes |

KGSL's comment on CM3_CFG: *"Make sure that CM3 state is at reset value.
Snapshot is changing NMI bit and if we boot up GMU with NMI bit set, GMU will
boot straight into NMI handler without executing __main code."*

Without it, the GMU boots into the NMI handler → executes unintended code →
reads unintended addresses → SMMU read faults at odd, varying IOVAs. This
matches everything: post-`gmu_start` timing (~2 ms after PRE_PDCSEQ covers
PDC writes + fw_load + CM3 regs + start), read direction, unmapped targets,
boot-to-boot variance.

Secondary: no GMU log buffer allocated and no `PWR_COL_CP_MSG` write (KGSL
`allocate_gmu_kmem(LOGMEM_SIZE)`). If the firmware logs unconditionally, same
class of fault. Address only if CM3_CFG is insufficient.

## 6. Decision-tree answers

* DDR added + fault identical → DDR not the cause; IOVA analysis prioritized —
  done above, converging on GMU-boot correctness (CM3_CFG), not mappings.
* Fault CB: our gmu_kernel domain (cbndx 1); SID 0x405 is a second master in
  its SMR group — consistent with a runaway GMU, not a missing CB.
* Fault IOVA vs mapped regions: outside all of them; static image fully
  covered; runtime-only → points at what the GMU EXECUTES, i.e. NMI boot.

## 7. Recommendation (no code changed this round)

Add ONLY `gmu_write(gmu, <CM3_CFG>, 0x4052)` before `a6xx_gmu_start()` in
`a6xx_gmu_fw_start()` (requires adding the `0x1F82D` register to our XML, per
KGSL `A6XX_GMU_CM3_CFG`). Single register write, explicit upstream
boot-correctness justification. Then re-test: if the GMU reaches HFI, the
bring-up is unblocked; if faults persist at fixed addresses, revisit the log
buffer next.