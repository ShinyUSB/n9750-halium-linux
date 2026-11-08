# Image 75 — GMU probe completes end-to-end

## Trace markers, in order (all 17 printed)

```
after memory_probe
after hfi alloc
before icache alloc_fixed      -> after icache alloc_fixed
before dcache alloc_fixed      -> after dcache alloc_fixed
before mmio map                -> after mmio map
after pdc_mmio map
after seq_mmio map
before irq requests            -> after hfi irq -> after gmu irq
before pwrlevels               -> after pwrlevels
before hfi_init                -> after hfi_init -- PROBE COMPLETE
```

Then:

```
msm_drm: loaded qcom/a630_sqe.fw from new location
msm_drm: loaded qcom/a640_gmu.bin from new location
```

So the A640 GMU probe, **including both fixed-IOVA cache allocations, all three
register mappings and both IRQ acquisitions, completes successfully**, and both
firmware blobs load.

This corrects the earlier suspicion: the fixed-IOVA `alloc_fixed` calls
(`iommu_map` bursts at 0x4000 / 0x44000) are **not** the failure point. They were
the prime suspect because the previous run died just after the OPP-table warning,
immediately before them. With markers in place they pass cleanly. That earlier
death was in fact the corrupt-filesystem regime.

## Device state

| | |
|---|---|
| panics / oops | **0** |
| reboots | **none** |
| EXT4 errors | **0** |
| uptime reached | 174 s and climbing |
| no D-state GPU thread | confirmed |

## Confirmed milestone state

```
ARM SMMU                       PASS
GPU address space (gfx3d)      PASS
A640 catalog/probe             PASS
GMU context bank (gmu_kernel)  PASS
GMU MMIO (all 3 regions)       PASS
GMU IRQs (both)                PASS
GMU probe                      PASS  (complete, synchronous, correct order)
a630_sqe.fw loaded             PASS
a640_gmu.bin loaded            PASS
kernel stable / no panic       PASS
```

## Remaining open question

No kernel message appears after `loaded qcom/a640_gmu.bin`. That is the last log
line before the attach would enter `msm_gpu_hw_init()` -> `a6xx_pm_resume()` ->
`a6xx_gmu_resume()`, which is where the real GMU bring-up happens
(`clk_bulk_prepare_enable`, `a6xx_gmu_irq_enable`, `a6xx_gmu_fw_start`,
`a6xx_hfi_start`).

There is no success or failure printk there, so we cannot yet distinguish:

* the attach **completed**, and there is simply nothing logged; or
* the attach is **stalled** inside `a6xx_gmu_resume` waiting on hardware.

The trigger `write()` did not return within 15 s, which favours "still running or
stalled", but that is inference, not proof.

Cannot be resolved from userspace: no compiler, no python, no debugfs on the
device, so `DRM_IOCTL_GPU_ID` cannot be issued and `/sys/kernel/debug/dri/0/` is
unavailable.

**Next step when approved:** add markers around `msm_gpu_hw_init()` /
`a6xx_gmu_resume()` stages to settle it.

## Note on capture

`/dev/kmsg` streaming worked and captured the full sequence, but stalled after
~8900 records while the device kept running. The 8192-line ring buffer plus
`sec_ts` spam (roughly 110 lines/s) rotates markers out within ~70 s, so markers
must be read promptly or the ring buffer's contents captured to the host
immediately.