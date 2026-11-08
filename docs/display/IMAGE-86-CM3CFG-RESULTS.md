# Image 86 — CM3_CFG did NOT fix it. Fault is SMMU read translation, GMU boot

## Runtime result

```
probe COMPLETE
fw[0] loaded (a630_sqe.fw)
fw[1] loaded (a640_gmu.bin)
enter a6xx_pm_resume
CX GDSC enable (ret=0, already=1)
clks enabled (ret=0)
GMU IRQ enabled
boot status read (cold)
before firmware start
SMMU gmu domain=... cb_dev=2c6a000.qcom,gmu:gmu_kernel fwspec SID[0]=0x4000005
SMMU map hfi    iova=0x60000000 size=0x4000 ok
SMMU map icache iova=0x4000     size=0x3c000 ok
SMMU map dcache iova=0x44000    size=0x3c000 ok
before CM3_CFG write (value=0x4052)   <- write succeeded
after CM3_CFG write                   <- marker printed
before GMU start                      <- marker printed
+288 us:
arm-smmu 2ca0000.kgsl-smmu: Unhandled context fault: iova=0x00089ce0, cb=1, fsr=0x40000402, fsynr0=0x60033, fsynr1=0x7
Kernel panic: No Device SMMU Fault - SID=0x405
```

## Key facts

* **CM3_CFG write itself succeeded** (marker after it printed).
* `a6xx_gmu_power_config()` ran clean between.
* **Identical fault address** (`0x00089ce0`) as image 85 — same one.
* Fault attribution (`No Device`, SID `0x405`) places the bad read on a GMU-side AHB/DMA master, i.e. the **GMU executing its firmware** and reading an address we did not map.
* The fault IOVA (`0x89ce0`) is not inside hfi/icache/dcache. It sits just past
  dcache (0x44000 + 0x3c000 = 0x80000; `0x89ce0 > 0x80000`). Across boots
  similar unmapped targets: `0x433c0`, `0x81ce0`, `0xb54e0`.

## What the firmware block map says vs our maps

`a640_gmu.bin` has 7 blocks: addresses `0x0,0x200,0x400` (ITCM, via CPU MMIO
writes, not DMA), `0x4000` (icache, covered), `0x40000,0x43fe0` (DTCM, via
MMIO writes), `0x57000` (dcache, covered). Statically every GMU-consumed block
lands inside a region we did map. So the GMU is reading somewhere the *static
image* does not indicate.

## Why this does not support a power/clock/DDR fix

The crash is a pure translation failure at a wrong runtime IOVA inside
`a6xx_gmu_start()`. Every power/clock/DDR resource needed is now enabled and
voted successfully (CX ret=0, DDR ret=0, 5 clocks ret=0). Adding more power
resources cannot create an IOVA mapping. This is now squarely an IOVA/firmware
memory-model problem.

## GSM-side open items (not derivable from the kernel tree)

To explain the offending runtime address we need the GMU-side view: what
datula/AHB decode does the bad read use, and against which program's memory map?
Candidates not yet resolved:

1. The GMU firmware expects the HFI table at a different IOVA/bus address than
   `gmu->hfi->iova = 0x60000000` that we voted.
2. The synthetic `dummy` page (declared in `a6xx_gmu` but **never allocated**
   in our tree) is expected by the firmware's block matcher and its absence is
   silently worked around elsewhere.
3. `itcm/dtcm` are not in the GMU SMMU window of this kernel generation; if the
   firmware uses them through its DMA path, those MMIO copies would fault.
4. Upstream v5.8 may keep icache/dcache at shifted IOVAs, requiring both
   `struct a6xx_gmu_bo` and the firmware's block addresses to change together.

No code changed this round. SDE/DT/IOMMU/PDC routing/parser/firmware/HFI/
fences/submit/Mesa/Weston untouched. Next step, when approved: obtain GMU
DBG/AHB fault context (Qualcomm internal docs or `a6xx_gmu_dump_fault` if it
exists in the KGSL tree) to get the firmware-side PC/address for `0x89ce0`.