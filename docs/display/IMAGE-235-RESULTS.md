# Image 235 — A640 clock values applied; the clock-gate theory is refuted

Image: `artifacts/flash/235-halium-boot-a6xx-a640clk.img`
sha256 `adb1ecae3fa8786aebc9844b9fe22c52d1e8e88c43348137f75f6020e6600982`
Kernel `#42`, boots `421ee889` and `2ad57c27`.

## What changed

CAF `adreno_a6xx.c:592-601` returns `0x8AA8AA02` for **a630 only**;
everything else, including a640, gets `0x8AA8AA82`. We had hard-coded the
a630 value, plus four HWCG entries carrying a630 constants.

| register | 234 | 235 | CAF a640 |
|---|---|---|---|
| `RBBM_CLOCK_CNTL` | `0x8aa8aa02` | **`0x8aa8aa82`** | `0x8aa8aa82` |
| `CLOCK_CNTL_SP0` | `0x22222222` | **`0x02222222`** | `0x02222222` |
| `CLOCK_CNTL2_SP0` | `0x02022220` | **`0x02222220`** | `0x02222220` |
| `CLOCK_CNTL2_RB0` | `0x00002222` | **`0x01002222`** | `0x01002222` |
| `CLOCK_CNTL_RAC` | `0x05022022` | **`0x05222022`** | `0x05222022` |

`a6xx_gpu.c:961` and the "already correct" test at `:1097` were keyed to the
old a630 constant, so they had been reporting success on a value that belongs
to a different chip. Both are now a640-aware.

Note: this tree has no `adreno_is_a630()` helper (only `adreno_is_a640()` at
`adreno_gpu.h:241`), so the selector is expressed in terms of a640.

## Bring-up is clean with the A640 values

```
[341.943847] ACD230: AOP mailbox channel acquired
[341.956119] ACD230: '{class: gpu, res: acd, value: 1}' len=32 mbox_send_message ret=0
[341.960775] FORENSIC VERDICT GOOD BABEFACE ms~1
[341.990173] HWINIT[03] GBIF/HALT: SKIPPED (image 233)
[341.990565] HWINIT[09] before a6xx_set_hwcg
[341.990631] HWINIT[10] after a6xx_set_hwcg RBBM_STATUS=0x1 STATUS3=0x0
[341.991818] HWINIT[25] return ret=0
```

The A640 clock value programs without wedging and `hw_init` returns 0.

## Result: no change. Clock gating is not the cause.

| measurement | 234 (a630) | 235 (a640) |
|---|---|---|
| `pixelreadback.py` | 0/6 | **0/6** |
| RGBA8 sweep | `nonzero=0/16384` | `nonzero=0/16384` |
| RGB8 sweep | `[0,0,0,255,...]` `4096/16384` | `[0,0,0,255,...]` `4096/16384` |
| R8 sweep | `[0,0,0,255,...]` `4096/16384` | `[0,0,0,255,...]` `4096/16384` |

Byte-identical outcomes. **The clock-gate hypothesis is refuted as the cause of
the blank render target.**

Retain the change regardless: writing a630 constants onto an a640 is wrong on
its face, and it is now correct.

## The 235 freeze was the deferral race, not the clock change

The first 235 boot froze on trigger. It was **not** caused by the clock change:

- `HWINIT[09]`/`[10]` (`a6xx_set_hwcg`) are at `a6xx_gpu.c:1449`.
- `ACD230` prints **before** `HWINIT[03]` (`:1335`), i.e. before the clock code.
- On the frozen boot neither `ACD230` nor `HWINIT[03]` ever appeared, so
  bring-up wedged *before* reaching the clock programming.

Separately, `IMAGE-221-CLOCKS.md` §0 documents that triggering **inside** the
300 s `gpu_init_delay_ms` window freezes the device. On this boot the first
detached trigger fired at ~240 s — inside the window — and blocked exactly as
documented. Re-triggering at 341 s (past it) worked first try.

**Operational rule, reconfirmed: never write `a6xx_gpu_load` before uptime
> 300 s.**

## Crash log lost again

`/proc/last_kmsg` again contained UEFI/recovery output (`Exit EBS`), not the
frozen kernel. `/sys/fs/pstore/` was empty — a **hang** writes no pstore, only a
panic does. So hang forensics still have no reliable channel; this remains
unresolved and is the biggest obstacle to iterating.

## Remaining CAF divergences (all verified, none invented)

Ranked by my own plausibility, none yet tested:

1. **LLCC / SMMU page-table attributes** (mimo #1). CAF sets
   `DOMAIN_ATTR_USE_LLC_NWA` for a640 (`kgsl_iommu.c:1270-1296`) and maps every
   buffer `IOMMU_READ|IOMMU_WRITE|IOMMU_NOEXEC` plus the LLC hint
   (`kgsl_iommu.c:1919-1948`). We map with plain `IOMMU_READ|IOMMU_WRITE`
   (`msm_gem_vma.c:236-238`), which yields `ATTRINDX=0` →
   `ARM_LPAE_MAIR_ATTR_NC`. CAF also activates the LLCC "gpu" slice and writes
   the SCID into `A6XX_GBIF_SCACHE_CNTL1` (`adreno_a6xx.c:1636-1656`); we have
   no equivalent. This is the only item that changes *where* GPU transactions
   are routed. Cannot determine what an unprogrammed SCID does.
2. **`CP_ME_INIT` mask** `0x2f` (ours) vs `0x16f` (CAF) — missing
   `OPERATION_MODE_MASK` and `REGISTER_INIT_LIST_WITH_SPINLOCK`, and no
   `pwrup_reglist` at all (`a6xx_gpu.c:1117-1133`).
3. **`RB_NC_MODE_CNTL` amsbc bit** — we write `0x4`, CAF writes `0x14` for a640
   per DT `qcom,ubwc-mode = <3>` (`adreno_a6xx.c:898-921`). Weak: `noubwc`
   writes also never land.
4. **`CP_ROQ_THRESHOLDS_2` / `PC_DBG_ECO_CNTL`** are a630 values on an a640
   (CAF `adreno_a6xx.c:834-835`, `:854-855`).
5. **GMU AO CGC registers** (`A6XX_GPU_GMU_AO_GMU_CGC_MODE/DELAY/HYST_CNTL`)
   — we have the defines but no writer; upstream master writes them.

## Refuted along the way (do not revisit)

- dma-iopmu identity domain — refuted five independent ways. Ours is a real
  arm-smmu domain: `arm-smmu.c:5159-5160` installs ops on the platform bus,
  `iommu.c:1288` uses `bus->iommu_ops`, and `arm-smmu.c:2352` overwrites the
  4 GB-1 clamp that the theory relied on. Empirically, IOVAs of 28 GiB on an
  11.2 GiB device cannot be physical addresses.
- `CP_SMMU_TABLE_UPDATE` — retracted in-corpus; requires Qualcomm `adreno_smmu`
  and is unreachable with a plain arm-smmu domain. It is a pagetable *switch*
  command and we have one pagetable for the device's lifetime.
- `adreno_program_smmu_aperture()` — CAF calls it only for `a640v1` (chipid
  patch 0). We are `qcom,adreno-640.1`, patch 1. Does not apply.
- `kgsl_mmu_get_bus()` resolving the SMMU master bus — that branch is
  `#ifdef CONFIG_QCOM_IOMMU`-gated and d2q has it **off**, so CAF returns
  `&platform_bus_type` exactly as we do.
- SQE firmware blob — no alternative exists on disk or device (the
  `a6xx-fw-orig` backup is byte-identical), so this is untestable without
  fabricating firmware. Left alone deliberately.

## Measurement caveat carried forward

Per `MEASUREMENT-VALIDITY-AUDIT.md`, zero pixels still does not prove the GPU
fails to render: the kernel BOS dump samples inside `a6xx_submit()` (before
execution) and `glReadPixels` is a GPU readback that depends on the subsystem
under test. A trustworthy verdict still requires reading the render target from
**CPU memory** via dmabuf (`gbm_bo` + `EGL_EXT_image_dma_buf_import` +
`GL_OES_EGL_image`), which needs no kernel work.