# Scope: mainline-msm control, and the three register items

Date: 2026-10-18

---

## Part 1 — Mainline msm control: SCOPED, and the honest verdict is "not feasible here"

### Facts established

| fact | value | source |
|---|---|---|
| this kernel | **4.14.190** | `Kernel/Makefile` |
| mainline a6xx merged | **5.7 / 5.8** | upstream tree history |
| msm driver total | 519 files, 1,873,435 lines | `find` |
| of which `adreno/` (the GPU) | **34,336 lines, 24 files** | `find` |
| of which `samsung/` | 1,298,737 lines | display extensions |
| of which `samsung_lego/` | 326,834 lines | display extensions |
| of which `sde/` | 74,996 lines | display |
| any tree on disk with mainline msm | **none** | checked ShinyKernel, ShinyKernel-gpu-exp, n9750-halium |

### Why a drop-in is not an option

Mainline `msm` for A6xx requires 5.x-era kernel APIs: modern DRM core
(`drm_driver` / per-context state / `drm_gem_object_funcs` evolution), the
post-4.14 DMA-BUF and DMA-API changes, and the IOMMU/`dma_resv` interfaces that
did not exist in 4.14. A6xx did not exist in any upstream 4.x driver, so there is
no older-mainline variant to start from. This is a DRM-core backport, not a
driver swap — realistically weeks of work with a low success probability, against
a project that has already produced eleven instruments with confident wrong
answers.

### What the scoping *did* change

The 1.87M-line figure that made the whole driver look intractable is **almost
entirely Samsung display code**. The actual GPU driver is 34K lines, and the
files that matter (`a6xx_gpu.c`, `a6xx_ringbuffer.c`, `msm_gem.c`, `msm_drv.c`)
are individually reviewable. That makes the **targeted upstream-vs-CAF diff** —
not a driver replacement — the practical instrument, and it is pure source work.

Cost so far: one upstream `a6xx_gpu.c` + `a6xx_catalog.c` fetch, and a scripted
register-write diff. Cheap, and it has already produced results.

### Cheaper control that is actually available

`freedreno_icd.aarch64.json` is installed, so **Turnip (native Vulkan) is
present**. That is a genuinely independent cmdstream generator — different
emitter, different state encoding, different cache handling from Gallium.

**However**, Turnip currently does **not enumerate the GPU**:
```
VK_ICD_FILENAMES=…/freedreno_icd…  vulkaninfo --summary
  -> only "llvmpipe (LLVM 15.0.6, 128 bits)"
```
while the Gallium EGL path works normally. So the control cannot be run until
Turnip's device probe is understood. Noted, not chased.

**Recommendation:** do not pursue mainline msm. Pursue the upstream diff, which
is the same idea (isolate CAF from hardware) at a fraction of the cost.

---

## Part 2 — The three register items

All three were re-derived against upstream Linux, not memory.

### 2. `CP_PROTECT_CNTL` — genuine divergence, but permissive-direction

Upstream (`a6xx_gpu.c:734-737`):
```c
gpu_write(gpu, REG_A6XX_CP_PROTECT_CNTL,
          A6XX_CP_PROTECT_CNTL_ACCESS_PROT_EN |
          A6XX_CP_PROTECT_CNTL_ACCESS_FAULT_ON_VIOL_EN |
          A6XX_CP_PROTECT_CNTL_LAST_SPAN_INF_RANGE);
```
Ours (`a6xx_gpu.c:2595`): `0x00000003` = the first two bits, **omitting
`LAST_SPAN_INF_RANGE`**.

Upstream's own comment: *"select the last span to protect from the start address
all the way to the end of the register address space."*

**Assessment: real, but it makes protection LESS strict, not more.** The last
protected span simply has a finite length instead of running to the top of the
register space. That cannot block a legitimate shader store — if anything it
permits more access. Our `CP_PROTECT(0..6)` table matches upstream's
`a630_protect` list (which upstream also uses for a640, per `a6xx_catalog.c`).

**Verdict: not a plausible cause. Do not spend a flash on it.**

### 3. `GBIF_SCACHE_CNTL0` / `_CNTL1` — ours is correct

Upstream programs `_CNTL0` **only** for `adreno_is_a660_family`
(`a6xx_gpu.c:1988-1990`). Our tree programs neither, which is correct for a640.

Upstream `_CNTL1`:
```c
cntl1_regval = (gpu_scid << 0) | (gpu_scid << 5) | (gpu_scid << 10) |
               (gpu_scid << 15) | (gpu_scid << 20);
gpu_rmw(gpu, REG_A6XX_GBIF_SCACHE_CNTL1, GENMASK(24, 0), cntl1_regval);
```
Ours (`a6xx_gpu.c:920-922`):
```c
for (i = 0; i < 5; i++)
        scid_val |= ((u32)(scid & 0x1f) << (i * 5));
gpu_write(gpu, REG_A6XX_GBIF_SCACHE_CNTL1, scid_val);
```
**Identical formula.** Ours uses a plain write rather than `rmw`, which zeroes
bits 24-31 instead of preserving them — a real but minor difference, and only
reachable through the `llcc_scid_set` module parameter, which is off by default.

**Verdict: not a defect. Not worth a flash.**

### 4. `PC_DBG_ECO_CNTL` — ours is exactly right

Upstream writes `adreno_gpu->info->a6xx->prim_fifo_threshold` if non-zero.
For A640 (`a6xx_catalog.c`):
```c
.chip_ids = ADRENO_CHIP_IDS(0x06040001),
.revn = 640,
.a6xx = &(const struct a6xx_info) {
        .hwcg = a640_hwcg,
        .protect = &a630_protect,
        .gmu_cgc_mode = 0x00020202,
        .prim_fifo_threshold = 0x00180000,
},
```
Ours (`a6xx_gpu.c:2581`): `(0x300 << 11)` = **0x00180000**.

**Identical. Verdict: not a defect.**

---

## Net result of Part 2

**Two of the three are exact matches with upstream; the third diverges in the
direction that would make the hardware *more* permissive, not less.**

That is a meaningful negative: the init path in these areas is not the problem,
and the three items are not worth flash cycles.

## Side observations (recorded, not acted on)

- **`ADRENO_QUIRK_4GB_VA`** is set on A640 upstream, but I could not locate its
  consumer in `a6xx_gpu.c` or `msm_drv.c` (and `mmu/msm_mmu.c` 404s at its old
  path). Our IOVAs sit at `0x7_00000000` ≈ 28 GB, well above 4 GB. This is worth
  locating, because if the GPU's effective IOVA aperture were 4 GB, shader-domain
  traffic could be discarded while the CP's own accesses happen to work. **Not
  currently supported by evidence** — the CP demonstrably writes at that address.
- Upstream A640 also carries `.quirks = ADRENO_QUIRK_HAS_CACHED_COHERENT |
  ADRENO_QUIRK_4GB_VA`; whether our tree implements the cached-coherent part is
  unexamined.

## What I would do next

Stop flash-per-register. The yield on that strategy is poor and we have just
demonstrated three near-misses in a row where our tree was already correct.

In order:
1. Locate the consumer of `ADRENO_QUIRK_4GB_VA` and check the actual SMMU
   aperture geometry our domain was given (`cb->domain->geometry.aperture_*` is
   already logged in `msm_gpu_mmu.c`). **Pure source/log reading, no flash.**
2. Understand why Turnip does not enumerate. **Pure userspace, no flash.**
3. Continue the scripted upstream diff beyond init — into `a6xx_ringbuffer.c`
   and the gem/mmu path — which is where a submission-path divergence would
   actually live, and which we have not diffed yet.

None of those three costs a build or risks the device.