# The dma-iopmu identity-mapping theory is REFUTED

Claim under test: `msm_gpu_cb_get()` calls
`iommu_domain_alloc(&platform_bus_type)` (msm_gpu_mmu.c:142), yielding
dma-iopmu (a pure identity mapper), so BO IOVAs are used as physical
addresses and render-target BOs map to the wrong physical memory.

This was the leading suspect for the all-zero renders, and **two independent
sub-agents reached the same conclusion**. It is wrong on this tree.

## Evidence A — the active SMMU registers on platform_bus_type

1. `drivers/base/platform.c:1147-1152` — `platform_bus_type` is initialised
   with **no** `.iommu_ops` (so `NULL` at boot).
2. `drivers/iommu/iommu.c:1224-1231` — `bus_set_iommu()`:
   ```c
   if (bus->iommu_ops != NULL)
           return -EBUSY;
   bus->iommu_ops = ops;
   ```
3. `drivers/iommu/arm-smmu.c:5156-5160` installs **arm-smmu's** ops there:
   ```c
   static void arm_smmu_bus_init(void)
   {
           if (!iommu_present(&platform_bus_type))
                   bus_set_iommu(&platform_bus_type, &arm_smmu_ops);
   ```
4. `drivers/iommu/iommu.c:1280-1292` — this CAF kernel uses
   `bus->iommu_ops` **directly**, not mainline's `bus->dma_ops->iommu_ops`:
   ```c
   if (bus == NULL || bus->iommu_ops == NULL)
           return NULL;
   domain = bus->iommu_ops->domain_alloc(type);
   domain->ops = bus->iommu_ops;
   ```

So `iommu_domain_alloc(&platform_bus_type)` returns a **real arm-smmu SMMU
domain**, not dma-iopmu. `dma-iommu.c` does exist in the tree but never
registers for platform bus.

**CORRECTION.** An earlier revision of this document cited
`arm-smmu-v3.c:2859` and `:1620`. That was wrong: the live config has
`# CONFIG_ARM_SMMU_V3 is not set`, so v3 is not compiled. The active file is
`arm-smmu.c` and the correct lines are `:5159-5160` and `:2352-2353` above.
The conclusion is unchanged — it is in fact stronger, because the clamp that
the theory relied on turns out to be dead code (Evidence B).

## Evidence B — the 4 GB-1 clamp is dead code

The theory leaned on "aperture printed `0x0-0x7fffffffff`, which is dma-iopmu's
39-bit default, whereas arm-smmu clamps to 4 GB-1." Two independent agents
cited that clamp as governing the observed value. It does not.

`drivers/iommu/arm-smmu.c:2298-2304`, inside
`if (!(smmu_domain->attributes & (1 << DOMAIN_ATTR_GEOMETRY)))`:

```c
domain->geometry.aperture_start = 0;
domain->geometry.aperture_end = (1UL << ias) - 1;
if (domain->geometry.aperture_end >= SZ_1G * 4ULL)
        domain->geometry.aperture_end = (SZ_1G * 4ULL) - 1;
```

and then **unconditionally**, 47 lines later, `arm-smmu.c:2352-2353`:

```c
domain->geometry.aperture_end = (1UL << ias) - 1;
domain->geometry.force_aperture = true;
```

The second assignment overwrites the first, so the 4 GB-1 clamp can never
affect the final value. At `ias = 39` the result is `(1<<39)-1` =
`0x7FFFFFFFFF` — **exactly the value observed**. The aperture was arm-smmu's
own default all along.

Note `aperture_start = 0` is set at `:2300` and is **not** re-set at `:2352`.
That is why the aperture came up starting at 0, and therefore why the image 234
rebase was genuinely **necessary** — memptrs was landing on IOVA 0 — even
though the originally stated reason was wrong.

## Evidence C — empirical, decisive, independent of all source reading

dma-iopmu is a *pure identity* mapper: IOVA == physical address. Therefore, if
the theory held, every GPU IOVA would be below the end of physical RAM.

Measured on the live device:

```
MemTotal: 11731188 kB          ~= 11.2 GiB physical RAM
rb_iova       = 0x700001000    =  28 GiB
memptrs_iova  = 0x700000000    =  28 GiB
```

A 28 GiB IOVA cannot be a physical address on a device with 11.2 GiB of RAM.
The IOVAs are therefore **translated virtual addresses backed by real SMMU
page tables**.

This also confirms the image 234 rebase is sound: it moved allocations into
the low part of the aperture, away from IOVA 0.

## Consequence

Address translation is NOT the defect. Corroborated independently by the
project corpus, which records successful GPU→DDR writes on the control plane:

- `IB-EXECUTES.md` — `CP_EVENT_WRITE` timestamps increment once per submit in
  data BOs (`0x16000`: 0x2→0x3→0x4; `0x20000`: 0x13→0x14)
- `IMAGE-213-RESULTS.md:26-33` — GPU wrote `rptr=114` into memptrs at IOVA 0,
  observed from the CPU
- `IMAGE-234` — fence echo works; recover spam 12→0

## What the evidence actually points at

Control-plane writes land; **render-backend (RB) and shader-pipe (SP) writes
have never once been observed**, across 2D-blit and shader-render-pass paths,
UBWC on and off, sysmem and gmem (`MATRIX-RESULT.md`, `IMAGE-221-CLOCKS.md`,
`IMAGE-234-RESULTS.md`).

The defect is downstream of address translation.

## Also verified (divergences found, deliberately NOT changed)

`CP_RB_CNTL` — upstream master `a6xx_gpu.c:1451-1456` writes it two ways:

```c
if (adreno_gpu->base.hw_apriv)
	gpu_write(gpu, REG_A6XX_CP_RB_CNTL, MSM_GPU_RB_CNTL_DEFAULT);
else
	gpu_write(gpu, REG_A6XX_CP_RB_CNTL,
		MSM_GPU_RB_CNTL_DEFAULT | AXXX_CP_RB_CNTL_NO_UPDATE);
if (a6xx_gpu->shadow_bo) { ...program CP_RB_RPTR_ADDR... }
```

CAF `adreno_a6xx.h:104` sets bit 27 (`0x08000000`) unconditionally. Our tree
writes plain `0x20c` and always programs the RPTR shadow. Confirmed on the
device: `RINGHW-W: CP_RB_CNTL after=0x20c`.

**Left alone deliberately.** Our fence echo works precisely *because* the CP
writes `memptrs->rptr`; `NO_UPDATE` would disable that mechanism. It is a real
divergence with unclear bearing on the RB/SP failure, and changing it risks
re-breaking a currently-working path.