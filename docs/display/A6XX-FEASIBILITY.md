# A6xx / Adreno 640 DRM bring-up — feasibility map (M12–M14)

Produced before any flashing, per task section 34. Nothing in the working system
was modified to produce this.

---

## STATUS: COMPILE-ONLY MILESTONE ACHIEVED — IOMMU ADAPTER DONE

**See `COMPILE-ONLY MILESTONE — ACHIEVED` and `IOMMU ADAPTER` near the end of
this document.**

A full kernel build links upstream v4.19 A6xx + the v5.8 A640 GMU/HFI-v2 path
+ a 268-line context-bank IOMMU adapter into this exact tree, with
`sde/`, `dsi/`, `dsi-staging/`, `samsung/`, `msm_iommu.c`, `msm_smmu.c` and
`msm_gem_vma.c` byte-for-byte unmodified.

The GPU SMMU turned out to be bound by the **generic** `arm-smmu.c`
(`qcom,qsmmu-v500`), not by KGSL, so no SMMU reimplementation was needed.

Nothing has been flashed. Firmware findings are in `GPU-FIRMWARE.md`.

---

## READ THIS FIRST — revision history

Sections `API INCOMPATIBILITIES`, `EXPECTED PATCH SIZE`, `RISK` and the first
`VERDICT SO FAR` were written from **inference about what the a6xx driver
probably needed**. They were then checked against real upstream source, and they
were **too pessimistic on every count**. They are retained below for traceability
but are superseded by the sections at the end of this document:

* `A6XX REFERENCE` → timeline corrected (A6xx is in mainline **4.19**, not 4.20)
* `ROOT CAUSE OF GPU_ID = ENXIO` → the real blocker, and it is small
* `DONOR MATRIX`, `RECOMMENDED DONOR`, `TRANSPLANT BOUNDARY`
* `SAMSUNG SDE RISK`, `FIRMWARE`, `BUILD-ONLY PLAN`, `RISK ASSESSMENT`, `VERDICT`

Two consultant reports were commissioned during this work; both contained
confabulated specifics, which are called out where relevant below. Nothing was
flashed and nothing in the working system was touched.

---

## CURRENT KERNEL

```
version:        4.14.190-ShinyKernel #2 SMP PREEMPT Wed Sep 30 16:41:22 +03 2026 aarch64
/proc/version:  Linux version 4.14.190-ShinyKernel (usb@shinymachine)
                (clang version 10.0.9, GNU ld (binutils-2.27-44492f8e) 2.27.0.20170315)
git:            NOT a git repository - no history, no remotes, no tags.
                Lineage must be inferred from content, not from git.
```

GPU: `Adreno640v2` (`/sys/class/kgsl/kgsl-3d0/gpu_model`), DT `qcom,chipid = <0x06040000>`.

Relevant build config (`artifacts/control-rebuild/out-control/.config`):

```
CONFIG_QCOM_KGSL=y
CONFIG_QCOM_KGSL_IOMMU=y
CONFIG_MSM_GPUCC_SM8150=y
CONFIG_DRM_MSM=y
(no CONFIG_DRM_MSM_GPU)
```

### MSM DRM generation: PRE-REBASE (4.14 CAF)

`drivers/gpu/drm/msm/msm_gpu.h` exposes the old callback set:

```c
struct msm_gpu_funcs {
	int   (*get_param)(...);
	int   (*hw_init)(struct msm_gpu *gpu);
	int   (*pm_suspend)(...);   int (*pm_resume)(...);
	void  (*submit)(...);       void (*flush)(...);
	irqreturn_t (*irq)(...);    uint32_t (*last_fence)(...);
	void  (*recover)(...);      void (*destroy)(...);
};
```

Absent (all required by upstream a6xx):

```
msm_gpu->info (adreno_info*)      : 0 occurrences
msm_gpu->num_gem_addresses        : 0
msm_gpu->num_iommu                : 0
msm_device_tensors                : 0
msm_iommu vgpu/kernel iommu split : 0 (only 1 hit in msm_mmu.h)
```

`msm_ringbuffer.h` is the old **one-ring-per-submit** model
(`msm_ringbuffer_new()` / `msm_ringbuffer_destroy()` / `OUT_RING()` macro) — not the
modern per-context attach/detach/get_timestamp/restore ringbuffer that a6xx and
the rebase require. `msm_ringbuffer_attach|get_timestamp` : 0 hits.

`adreno/` contains a2xx, a3xx, a4xx, a5xx only. No a6xx. The `adreno_device`
table has `A530` and `adreno`; no A640 entry.

### Integration point already present

This is the single most useful finding. `msm_drv.c` (2262 lines) already does:

```c
msm_drv.c:837:   if (!priv->gpu)
msm_drv.c:838:           priv->gpu = adreno_load_gpu(dev);
```

and `adreno/adreno_device.c` provides `set_gpu_pdev()` and the
`adreno_load_gpu()` loader. The plumbing to attach a GPU to the DRM device is
present; `priv->gpu` is NULL today only because **no A640 GPU ever binds** (no
upstream-vocabulary DT node, no A640 in the adreno_info table, no a6xx driver),
so `GETPARAM GPU_ID` returns `-ENXIO` at `msm_drv.c:1143`.

### KGSL generation: full A6xx, INCLUDING GMU

KGSL in this same tree already drives the A640 and contains an A6xx
implementation plus GMU/HFI infrastructure that upstream's a6xx needs:

```
drivers/gpu/msm/adreno_a6xx.c          (A6xx core)
drivers/gpu/msm/adreno_a6xx_gmu.c      (A6xx GMU)
drivers/gpu/msm/adreno_a6xx_rgmu.c     (A6xx rubber-GMU)
drivers/gpu/msm/kgsl_gmu.c             (GMU core)
drivers/gpu/msm/kgsl_gmu_core.c/.h
drivers/gpu/msm/kgsl_hfi.c/.h          (HFI)
```

This is the "hardware map" the task asks for (section 11), and it is unusually
good: the GMU protocol, HFI, and A6xx specifics are already implemented and
proven on this exact SoC.

---

## HARDWARE RESOURCE MAP

From `arch/arm64/boot/dts/qcom/sm8150-gpu.dtsi` (the node the d2q overlay pulls in):

```
Resource            Vendor KGSL (present)                    Upstream DRM A640        Match
------------------------------------------------------------------------------------------------
GPU base            0x2c00000 len 0x40000                    0x02c00000              YES
GPU CX/debug        0x2c61000 len 0x800                      (n/a)                   -
QDSS_GFX            0x6900000 len 0x44000                     (n/a)                   -
QFPROM              0x780000 len 0x6fff                       (n/a)                   -
GPU interrupt       300                                      GIC_SPI 300             YES
GMU base            0x2c6a000 len 0x30000                    0x02c6a000              YES
GMU PDC cfg/seq     0xb280000 / 0xb480000                    -                        -
GMU interrupts      304 (kgsl_hfi_irq), 305 (kgsl_gmu_irq)   304, 305                YES
IOMMU               kgsl-iommu@0x02CA0000 (smmu-v2)           adreno_smmu iommu@2ca0000  YES
IOMMU CBs           gfx3d_user 0x0 (+0xa8000), secure 0x2,    upstream iommus          YES (0x0/0x401
                    gmu_user 0x4, gmu_kernel 0x5             = <&smmu 0 0x401>       matches exactly)
GPUCC               GPU_CC_CXO_CLK, GPU_CC_CX_GMU_CLK,      qcom,sm8150-gpucc       PARTIAL - we have
                    GPU_CC_AHB_CLK (+ DDRSS_GPU_AXI,          (CAF driver only)       CAF MSM_GPUCC_SM8150
                    GPU_MEMNOC_GFX, L3_GPU_VOTE)                                      no upstream gpucc
GPU power domains   gpu_cx_gdsc (vddcx), gpu_gx_gdsc (vdd)   same                     YES
OPP table           215/309/379/486/553/600 MHz              gpu_opp_table            YES (same values)
Interconnect        gpubw (26/512), CNOC (26/10036)          same                     YES
ZAP firmware        pil_gpu "qcom,pil-tz-generic"           a640_zap.mdt             DIFFERENT MECHANISM
                    pas-id 13, fw-name "a640_zap"            (loaded by driver)       (loaded by TrustZone)
```

**Conclusion of the resource map (task section 11):** the hardware description
already exists and is, at the address/IRQ/SMMU level, identical to upstream. The
addresses line up one-for-one with the upstream SM8150 reference, including the
`iommus = <&adreno_smmu 0 0x401>` GPU mapping which here is spelled
`<&kgsl_smmu 0x0 0x401>` on the `gfx3d_user` context bank.

**The missing piece is not hardware. It is the DRM/MSM GPU software layer.**

Two resource caveats that are genuine differences, not just spelling:

1. **ZAP loading mechanism.** KGSL's ZAP is loaded by TrustZone via a `pil`
   (`qcom,pil-tz-generic`, pas-id 13). Upstream a6xx loads ZAP itself from
   `request_firmware()` using a DT `qcom,zapfw-name`. Nothing in our tree provides
   the latter.
2. **Clock/SMMU drivers are CAF vendor implementations**, not the upstream
   `qcom,sm8150-gpucc` / `arm,mmu-500`-style bindings a6xx expects. The DT nodes
   use vendor compatibles (`qcom,kgsl-3d0`, `qcom,gpu-gmu`, `qcom,kgsl-smmu-v2`),
   whereas upstream a6xx matches `qcom,adreno-640.1`/`qcom,adreno` and
   `qcom,adreno-gmu-640.1`/`qcom,adreno-gmu`.

---

## A6XX REFERENCE

```
source version:  upstream Linux. A6xx merged in 2018 (commit 4b565ca5a2cb per
                 task section 3).
chosen implementation: NOT YET CHOSEN - this is the pivotal decision below.
reason:            Our base (4.14) PREDATES a6xx and, more importantly, predates
                 the "MSM GPU rebase" that a6xx was written against. A6xx cannot
                 be compiled onto this msm_gpu API.
```

### Timeline correction (verified 2026-10-02)

An earlier draft of this document said "A6xx merged in ~4.20". **That was wrong
and is withdrawn.** Verified directly against the mainline release tag `v4.19`
via the GitHub contents API - the complete A6xx implementation is already
present there:

```
drivers/gpu/drm/msm/adreno/ @ v4.19 (mainline release, Dec 2018)
  a6xx_gpu.c      26823 bytes
  a6xx_gpu.h       1565
  a6xx_gmu.c      30749
  a6xx_gmu.h       3943
  a6xx_gmu.xml.h  14297
  a6xx_hfi.c      10878
  a6xx_hfi.h       2382
  a6xx.xml.h     167771
  adreno_device.c  9505
  adreno_gpu.c    19250
  adreno_gpu.h    11150
```

So A6xx + GMU + HFI shipped in mainline **4.19**, and the 4.19 stable series
carries further A6xx fixes. A6xx is old, well-established upstream code - not an
active research problem. The difficulty here is purely the age of OUR tree.

### Our adreno core vs upstream v4.19 (measured)

```
file                 ours (4.14 CAF)   v4.19 upstream   delta
adreno_device.c           9091              9505         structurally DIFFERENT
adreno_gpu.c             12111             19250         +59%
adreno_gpu.h              9951             11150         +12%
```

They are not merely older, they are *different in kind*. Ours selects the GPU via
the legacy CAF mechanism:

```c
struct adreno_platform_config *config;
config = pdev->dev.platform_data;      /* adreno_device.c:138 */
```

whereas v4.19's `adreno_load_gpu()` derives the chip ID from the DT `compatible`
string via `find_chipid()` / `of_property_read_string_index()`. So even the
"identical-looking" loader files are not drop-in replaceable, and the adreno core
is entangled with which `msm_gpu` generation is present.

### The API boundary that decides strategy

Upstream a6xx is not a standalone driver. It sits on the **post-4.16 MSM GPU
rebase**, which changed the whole GPU-side architecture. Specifically a6xx calls:

```
msm_gpu_funcs:  gpu_init, gpu_hw_init, load_firmware, init_iommu,
                enable_iommu, disable_iommu, idle, active
msm_gpu->info, msm_gpu->num_gem_addresses, msm_gpu->num_iommu
msm_iommu with kernel+vgpu split, msm_device_tensors
modern msm_ringbuffer (attach/detach/get_timestamp/restore)
reworked msm_gem_submit (no retired_submit)
```

Our tree has **none** of these (counts in the table above are all zero). So:

* **Strategy A (minimal a6xx backport) is NOT viable.** a6xx will not compile
  against this `msm_gpu`, and fixing that means importing the rebase anyway.
* The real choice is therefore between:
  * **A': rebase + a6xx hand-ported into this vendor tree**, keeping Samsung SDE.
  * **B': take a coherent CAF donor tree that already contains BOTH SDE display
    and a6xx DRM** (Qualcomm CAF added adreno a6xx to DRM/MSM around 2019-2020,
    i.e. CAF 4.19), and lift only the GPU subsystem across.

  B' is materially safer: the rebase and a6xx were developed together against
  each other, and a CAF donor has the same SDE display lineage as our tree, so the
  display side stays coherent.

---

## REQUIRED NEW COMPONENTS

```
a6xx_gpu            : MISSING - must add (~3-4k lines with headers)
a6xx_gmu            : MISSING - must add (upstream GMU; KGSL's rGMU exists as
                       a hardware reference but is not API-compatible)
a6xx_hfi            : MISSING - must add (KGSL's kgsl_hfi.c is the reference)
register definitions: MISSING - a6xx generated headers
adreno integration  : A640 entry MISSING in adreno_info table; adreno_load_gpu()
                      exists and would work if a GPU bound
CONFIG_DRM_MSM_GPU  : MISSING (not set)
IOMMU               : vendor kgsl-smmu-v2 present; upstream adreno-smmu binding ABSENT
GPUCC               : CAF MSM_GPUCC_SM8150 present; upstream qcom,sm8150-gpucc ABSENT
power domains       : PRESENT (gpu_cx_gdsc, gpu_gx_gdsc)
OPP                 : PRESENT (215-600 MHz)
reserved memory     : PRESENT (qcom,gpu-mempools in DT)
GMU firmware        : ABSENT - need a640_gmu.bin
ZAP firmware        : ABSENT - need a640_zap.mdt (Samsung's is TZ-loaded, different path)
SQE firmware        : ABSENT - need a630_sqe.fw
```

Firmware search so far: no `a640*`, `*zap*`, `*gmu*`, `*sqe*` anywhere in
`artifacts/`, on the phone, or in `Загрузки/` (which holds
`SAMFW.COM_SM-N9750_TGY_N9750ZSU6HZE1_fac.zip` - not yet opened, likely source).

---

## VENDOR RESOURCES ALREADY PRESENT

- Complete A6xx driver incl. GMU/rGMU/HFI, in KGSL form.
- Working A640 clock/power/IOMMU/OPP/interconnect description in
  `sm8150-gpu.dtsi`, matching upstream addresses exactly.
- `msm_drv.c` GPU attach path (`adreno_load_gpu`, `set_gpu_pdev`).
- Samsung SDE display path, untouched and working.
- android-6.12 SM8150 upstream reference and the OnePlus 7T Pro bring-up
  precedent (task sections 4, 5) for the architecture and firmware names.

---

## MISSING

Everything in the a6xx software column above, plus all three firmware blobs,
plus a DT expressed in upstream vocabulary, plus the entire post-4.16 MSM GPU
rebase API surface.

---

## API INCOMPATIBILITIES

Single root cause: **this tree's msm_gpu is pre-rebase (4.14 CAF).** Every
a6xx dependency listed above is a consequence of that. There is no known way to
add a6xx to this tree without also importing the rebase, because the rebase
rewrote `msm_gpu.c/h`, `msm_drv.c` (GPU half), `msm_iommu.c`,
`msm_ringbuffer.c/h`, `msm_gem_submit.c`, and the adreno core - i.e. the same
files that carry Samsung's SDE display integration.

---

## EXPECTED PATCH SIZE

```
a6xx (core+gmu+hfi+headers)      : ~5,000 lines (task: 13 files / ~4,800 lines)
MSM GPU rebase (msm_gpu, msm_drv GPU half, msm_iommu,
  msm_ringbuffer, msm_gem_submit, adreno core, msm_kms.h)  : ~8,000-15,000 lines
DT nodes in upstream vocabulary   :   ~300-500 lines
firmware blobs                    : 3 binaries (a630_sqe.fw, a640_gmu.bin, a640_zap.mdt)
------------------------------------------------------------------
Realistic total (hand-ported)     : 15,000-25,000 lines, touching the files that
                                    implement the working display path
```

---

## RISK

**HIGH.** Concretely:

1. The MSM GPU rebase overlaps `msm_drv.c` / `msm_kms.h`, which are the SDE
   display files. A mistake there risks the *display*, which is the only thing
   keeping this headless device usable. That is a direct hit on the task's stop
   conditions ("the display path becomes unstable", "boot reliability is lost").
2. ZAP firmware is signed/secure-world-provisioned on Samsung parts. Upstream
   a6xx expects to load ZAP itself. If Samsung's `a640_zap` cannot be consumed by
   `request_firmware()` as `a640_zap.mdt`, the GPU cannot start at all, and this
   is not discoverable without booting Android and inspecting the actual blob.
3. 15k-25k lines of churn is far past "smallest safe backport"; it is the
   scenario the task explicitly warns against ("the experimental patchset becomes
   an uncontrolled rewrite of the kernel").

Mitigation that keeps this honest: build an **experimental image on a separate
output path**, keep image 62 (`e19c71fa279af08af4eba97be4b27a3f2ee9af5ae93454406ffa90f5ed635809`)
as the recovery artifact, and verify boot + fbcon + Weston before anything else.

---

## ROOT CAUSE OF GPU_ID = ENXIO (verified in our tree, 2026-10-02)

This was not a bug, and not a missing-driver symptom. Our `msm_drv.c`
**deliberately compiles the DRM GPU path out** when KGSL is enabled:

```c
/* msm_drv.c:825 */
#ifdef CONFIG_QCOM_KGSL
static void load_gpu(struct drm_device *dev)
{
}                                       /* <-- EMPTY */
#else
static void load_gpu(struct drm_device *dev)
{
        static DEFINE_MUTEX(init_lock);
        struct msm_drm_private *priv = dev->dev_private;
        mutex_lock(&init_lock);
        if (!priv->gpu)
                priv->gpu = adreno_load_gpu(dev);   /* <-- the real path */
        mutex_unlock(&init_lock);
}
#endif

/* msm_drv.c:2095 */
#ifdef CONFIG_QCOM_KGSL
static int add_gpu_components(struct device *dev, struct component_match **matchptr)
{
        return 0;                        /* <-- NO-OP */
}
#else
static int add_gpu_components(...)
{
        struct device_node *np;
        np = of_find_matching_node(NULL, msm_gpu_match);
        ...
}
#endif
```

And the matcher **already accepts upstream-compatible DT strings**:

```c
/* msm_drv.c:2088 */
static const struct of_device_id msm_gpu_match[] = {
        { .compatible = "qcom,adreno" },
        { .compatible = "qcom,adreno-3xx" },
        { .compatible = "qcom,kgsl-3d0" },
        { },
};
```

`load_gpu()` is invoked from `msm_open()` with the comment *"Now, load gpu on
open.. to avoid the requirement of having firmware in the initrd."*

**Implication:** the GPU attach machinery is fully present and functional in this
tree. It is gated by exactly one `#ifdef CONFIG_QCOM_KGSL`. Enabling the DRM GPU
is a matter of turning KGSL off, not of writing an attach path. That single
change is the highest-value item in this whole task.

## DONOR MATRIX (section 12 deliverable)

No single tree has both SDE and A6xx. Verified by sweeping ~100 Qualcomm Android
kernel repos: the split is clean along CAF lineage (SDE was renamed to MDP in
4.19; A6xx was only added from 4.19 onward). So donors are used per-axis.

| # | Donor | Ref / SHA | Kernel | a6xx | sde/ | SM8150 dtsi + `gpu@2c00000` "qcom,adreno-640.1" | msm_gpu_funcs gen |
|---|---|---|---|---|---|---|---|
| 1 | **CAF `msm-4.14`** | `LA.UM.9.1.1.c25` = `4c6be0458e26378af5628a49c5aa34a8e2d2be49` | **4.14.190** | no | **yes (87 files)** | sm8150 dtsi yes, but GPU node is `qcom,kgsl-3d0@2C00000`, no DRM node | OLD |
| 2 | **CAF `msm-4.19`** | `LA.UM.9.15.1.c27` = `fdcf1cf145712b0f1cdcf4ac2c7a755e9605d79a` | 4.19.157 | **yes** | no (`disp/`=mdp) | no sm8150 dtsi | OLD (same as upstream v4.19) |
| 3 | upstream `torvalds/linux` | tag `v4.19` | 4.19 | **yes** | no | no | OLD |
| 4 | upstream `gregkh/linux` | `v4.19.y` | 4.19.y | yes + stable fixes | no | no | OLD |
| 5 | CAF `msm-5.4` | `LA.UM.10.9.1.c25` = `b1927563deee034ce7b6042b123d3194ec6117a8` | 5.4.259 | yes | no | sm8150 dtsi, no GPU node | OLD + gpu_get_freq/set_freq |
| 6 | upstream `torvalds/linux` | tag `v5.10` | 5.10 | yes | no | **only real source of the A640 node** | OLD |

Verified independently (against the GitHub contents API): CAF 4.19's
`a6xx_gpu.c` / `a6xx_gpu.h` / `adreno_gpu.h` are **md5-identical to upstream
v4.19**, and upstream v4.19's `msm_gpu_funcs` is member-for-member what was
fetched. Also verified: the "`gpu_init` / `load_firmware` / `init_iommu`
generation" of `msm_gpu_funcs` **does not exist in upstream at any version**
(checked v4.14 through v6.6) - upstream v6.6 still declares
`int (*hw_init)(struct msm_gpu *)`. That idea was a mistake and is discarded.

## RECOMMENDED DONOR

```
A6xx source : upstream torvalds/linux tag v4.19 (or CAF msm-4.19
              LA.UM.9.15.1.c27 - proven md5-identical)
DTS + A640   : upstream torvalds/linux tag v5.10  (sm8150.dtsi gpu@2c00000 node;
               "qcom,adreno-640.1" first appears in v5.10, revn=640 in v5.9)
base        : our own tree (CAF 4.14.190, = CAF LA.UM.9.1.1.c25)
```

Not CAF 5.4, not upstream >= 5.4: both drag in DPU-era and DSM rework we neither
have nor want, and since upstream A6xx never adopted a "new gen" API there is
nothing to gain from going newer.

## TRANSPLANT BOUNDARY

```
ADD verbatim from v4.19 (~5k lines, no adaptation needed):
  adreno/a6xx_gpu.c  a6xx_gpu.h
  adreno/a6xx_gmu.c  a6xx_gmu.h  a6xx_gmu.xml.h
  adreno/a6xx_hfi.c  a6xx_hfi.h  a6xx.xml.h

MERGE (hand-edit, GPU-side only; SDE touches none of these):
  msm_gpu.h / msm_gpu.c      add rb[MSM_GPU_MAX_RINGS] + nr_rings,
                             msm_gpu_submitqueue, msm_gpu_state, retire_work;
                             funcs += active_ring, gpu_busy, gpu_state_get/put,
                             debugfs_init; flush() gains a ring arg;
                             show() becomes (gpu, msm_gpu_state*, drm_printer*);
                             add gpu->info, num_gem_addresses, num_iommu,
                             device_tensors;
                             *** PRESERVE vendor-only gpu->fctx ***
  msm_ringbuffer.h / .c     add id, preempt_lock, memptrs, restore_wptr,
                             next_buffer, seqno, nr_free, timestamp
  msm_gem.h / msm_gem.c     msm_gem_submit gains file, runnable, queue
  msm_iommu.c               add msm_iommu_pagetable_params()
  adreno/adreno_device.c    add A640 entry (chip_id 0x06040001,
                             fw a630_sqe.fw + a640_gmu.bin, gmem SZ_1M,
                             zapfw a640_zap.mdt)
  adreno/adreno_gpu.h/.c    fw[] array + ADRENO_FW_* enums, adreno_request_fw(),
                             adreno_fw_create_bo(), init signature change
  msm_drv.c                 remove the CONFIG_QCOM_KGSL no-op guards  <-- key
  DT                        new gpu@2c00000 (upstream vocabulary) + gmu@2c6a000
                            + zap-shader node + adreno_smmu binding

DO NOT TOUCH:
  sde/  dsi/  samsung/  msm_kms.h  msm_fb*.c  msm_drm_overlay.c
```

Note: `a6xx_show()` in the 4.19 tree takes a `struct msm_gpu_state *`, which our
`msm_gpu` lacks (no `crashstate`/`devfreq`). Either stub it or fold in the
`msm_gpu_state` struct. That is the only CAF-4.19-era dependency inside the A6xx
files themselves.

## SAMSUNG SDE RISK - measured, and LOW

` sde/` and `dsi/` were searched for every dependency on the GPU layer. The
complete list of MSM GPU/MMU internals they use is:

```
sde/sde_kms.c:891    mmu->funcs->one_to_one_map()      splash screen
sde/sde_kms.c:952    mmu->funcs->one_to_one_unmap()    splash screen
sde/sde_kms.c:1976   aspace->mmu->funcs->is_domain_secure()
sde/sde_kms.c:1983   aspace->mmu->funcs->detach()
sde/sde_kms.c:2009   aspace->mmu->funcs->is_domain_secure()
sde/sde_kms.c:2013   aspace->mmu->funcs->attach()
```

Zero accesses to `priv->gpu->`, `gpu->rb`, or `msm_ringbuffer`. The other ~25
`->funcs` hits are DRM/bridge plumbing (`encoder->bridge->funcs`,
`priv->crtcs[i]->funcs`), not the MSM GPU layer.

**Decisively: our `msm_mmu_funcs` is already the full 15-pointer generation** -
identical to v4.19 - containing all five of those functions with matching
signatures. So the SDE display path depends on none of the GPU-layer pieces being
replaced. That is why replacing the GPU layer does not require rebasing SDE.

`msm_drv.c` itself needs only `gpu->funcs->{pm_suspend,destroy,get_param}`,
`gpu->aspace` (all present in v4.19) and `gpu->fctx` (vendor-only - carry it).

The real non-GPU risk is the **IOMMU context-bank assignment**, not SDE:
upstream's node points `iommus = <&adreno_smmu 0 0x401>` at upstream's SMMU-v2
`qcom,sm8150-smmu-500`, whereas our CAF tree uses `msm-arm-smmu-sm8150.dtsi` with
`arm,mmu-500` + `qcom,kgsl-smmu` bindings, where CB `0x401` is likely already
claimed by KGSL. Reconciling that is DTS/SMMU work and is the hardest remaining
piece.

## FIRMWARE

| source | filename | format | consumer | usable by upstream a6xx? |
|---|---|---|---|---|
| upstream linux-firmware | `qcom/a630_sqe.fw` | raw | DRM a6xx | yes |
| upstream linux-firmware | `qcom/a640_gmu.bin` | raw | DRM a6xx | yes |
| upstream linux-firmware | `qcom/a640_zap.mdt` (+ b00/b01/b02) | MDT | DRM a6xx | yes |
| Samsung stock | - | - | KGSL via TZ/PIL | **NOT LOCALLY AVAILABLE** |

**Verified:** `Загрузки/SAMFW.COM_SM-N9750_TGY_N9750ZSU6HZE1_fac.zip` (6.3 GB)
contains only five Odin `.tar.md5` manifests (BL/AP/CP/CSC/HOME_CSC). No firmware
images whatsoever, so Samsung GPU firmware cannot be obtained from it. The phone
itself has none of these files under `/lib/firmware` or `/etc/firmware`.

ZAP mechanism difference (confirmed by both the DT and the consultants): upstream
loads ZAP itself via `request_firmware()` + `qcom_mdt_load()` +
`qcom_pas_auth_and_reset()` using `firmware-name` on a `zap-shader` child node.
Samsung/KGSL loads it through TrustZone/PIL (`qcom,pil-tz-generic`,
`qcom,pas-id = <13>`). Both use PAS id 13. Whether a Samsung-signed `.mbn` would
satisfy upstream's MDT parser is **unverified**; an MBN wraps the MDT so it is
plausible. Untested option: boot Android and read `/vendor/firmware` for
`a630_sqe.fw` / `a640_gmu.bin`.

Chipid discrepancy to resolve: our DT says `qcom,chipid = <0x06040000>`; upstream's
A640 table uses `0x06040001`. The A640 table entry must be authored by us - no
4.14/4.19/5.4/5.8 tree has it.

## BUILD-ONLY PLAN (no flashing)

```
1. Copy tree -> ShinyKernel-gpu-exp; keep ShinyKernel untouched.
   Recovery image stays 62-halium-boot-bt-public.img
   (e19c71fa279af08af4eba97be4b27a3f2ee9af5ae93454406ffa90f5ed635809).

2. Fetch v4.19 adreno/{a6xx_gpu,a6xx_gmu,a6xx_hfi}{.c,.h} + a6xx*.xml.h verbatim.
   Fetch v5.10 sm8150.dtsi gpu@2c00000 node for reference only.

3. Merge msm_gpu.h/.c, msm_ringbuffer.h/.c, msm_gem.h, msm_iommu.c for the
   deltas listed above, PRESERVING gpu->fctx.

4. Add A640 entry to adreno_device.c; add ADRENO_FW_* to adreno_gpu.h.

5. Author the DT: gpu@2c00000 (compatible "qcom,adreno-640.1","qcom,adreno"),
   gmu@2c6a000 with zap-shader, adreno_smmu binding, reusing our existing
   gpu_opp_table / gpu_cx_gdsc / gpu_gx_gdsc / gpubw nodes.

6. Enable CONFIG_DRM_MSM_GPU. Remove the #ifdef CONFIG_QCOM_KGSL no-op guards
   in msm_drv.c.

7. Build to /dev/null. Iterate on compile errors ONLY. No flashing, no runtime
   change to the working tree.

Milestone gates before any flashing:
   M15 kernel builds with A6xx      (step 7 done)
   M16 A640 detected, GMU up, fw loaded, IOMMU live, display intact
   M17 renderD128 backed by a real msm_gpu; GPU_ID no longer ENXIO
   M18 real command submission + fence
```

## RISK ASSESSMENT (per subsystem)

```
MSM core merge      medium   bounded delta, 5 files, measured against v4.19
A6xx driver         low      verbatim upstream, no adaptation needed
GMU                 low      same (part of the a6xx set)
IOMMU               high     CB 0x401 already claimed by KGSL; needs reconciliation
firmware            medium   linux-firmware exists, but ZAP provenance/signing unverified
ZAP                 high     different load path; .mbn vs .mdt unverified
SDE integration     low      coupling is 5 mmu callbacks we already have
boot                low      no Kconfig/structural change that should affect boot
```

No overall score. The two genuine `high` items are **IOMMU context banks** and
**ZAP provenance** - both outside the GPU driver proper, and both independent of
SDE.

## CONSULTANT REPORTS - ERRORS FOUND AND CORRECTED

Three sub-agents were consulted in parallel. Two contained claims that do not
survive checking. They are recorded here so they are not repeated.

**1. Dependency-analysis report.** Asserted that A6xx requires `drm_gpuvm`,
`msm_gem_kernel_new`, `active_submits`, `hw_apriv` and commit `5a0e9e9`
("MSM GPU rebase"), and recommended a ~10-file "compatibility shim layer".

*Verdict: wrong.* Those are **5.x/master** concepts; none exist in v4.19, which is
the tree that actually contains A6xx. The SHA was self-admittedly unverified. The
remedy was also incoherent: a shim that returns `-ENOTSUPP` for page-table
programming cannot yield a working GPU, and a `drm_gpuvm` shim in a 4.14 tree is
not a small thing. Its framing of which `msm_gpu_funcs` generation A6xx targets
was internally inconsistent.

**2. Firmware report.** Claimed to have inspected the Samsung stock firmware and
found `a630_sqe.fw` and `a640_gmu.bin` in `/firmware`, and asserted the OnePlus
`hotdog` bring-up had used a `.mbn` ZAP successfully.

*Verdict: the firmware-content claim is false.* The 6.3 GB Samsung zip contains
only five Odin `.tar.md5` manifests - no firmware images at all. The `hotdog`
repository and its evidence file could not be confirmed, so the `.mbn`-works
conclusion is **unverified**. The ZAP *mechanism* analysis in that report was
sound and matches our DT reading, and is retained above.

**3. Donor-hunt report.** md5-verified that CAF 4.19's a6xx files are identical
to upstream v4.19, swept ~100 Qualcomm Android kernel repos, and produced concrete
SHAs. It also independently confirmed that the `gpu_init`/`load_firmware`
generation of `msm_gpu_funcs` exists nowhere in upstream, including v6.6.

*Verdict: reliable, and it corroborated the corrected analysis.* Its three
tree-specific warnings were also checked locally and hold: the KGSL no-op guards
exist, and the `#else` branches are complete.

---

## COMPILE-ONLY MILESTONE — ACHIEVED (2026-10-02)

Answering the question that was asked, from a successful compile rather than
from intuition:

> **Can we build a coherent A6xx DRM/MSM driver into this exact Samsung kernel
> without rebasing/replacing the working SDE?**
>
> **Yes. Proven by a full kernel build that links A6xx and leaves SDE
> byte-for-byte untouched.**

```
Experimental tree : /home/usb/ShinyKernel-gpu-exp/Kernel   (1.3 GB)
Build output      : /home/usb/mnt-sda/a6xx-build/out-gpu
Config fragment   : configs/d2q_a6xx_drm_experimental.config
Production tree   : UNTOUCHED (verified: no files modified today,
                    recovery image 62 intact, CONFIG_QCOM_KGSL=y retained)

Image        53fb7b620eaa846ccb11373a5f43c958ce6b0e58c01806cd35b5e9b500dc3a69
Image-dtb    8ae46728d0bc7832b17d00e7eca6c92b1b40dcc841d5ee4700979d3c621a95d9
.config      c77676e455cc2ebb36d373a25d435a5e7eb0d960b3fd8012f73f9a1325cb6621
```

### Donor, precisely

```
A6xx source : upstream torvalds/linux, tag v4.19
              (CAF msm-4.19 LA.UM.9.15.1.c27 is md5-identical and equally usable)
Base        : this tree, CAF 4.14.190
```

A6xx files copied **verbatim** (7,753 lines, zero adaptation):

```
adreno/a6xx_gpu.c      a6xx_gpu.h        26,823 / 1,565
adreno/a6xx_gmu.c      a6xx_gmu.h         30,749 / 3,943
adreno/a6xx_gmu.xml.h                      14,297
adreno/a6xx_hfi.c      a6xx_hfi.h         10,878 / 2,382
adreno/a6xx.xml.h                         167,771
```

### Diffstat

```
22 files changed, 8254 insertions(+), 76 deletions(-)   in drivers/gpu/drm/msm
  of which 7753 insertions are the verbatim a6xx files
  hand-written merge: ~501 insertions, 76 deletions across 14 files
 1 file changed, 14 insertions(+)   arch/arm64/boot/dts/qcom/sm8150-gpu.dtsi

sde/ ............ 0 differences      dsi-staging/ ....... 0 differences
dsi/ ............ 0 differences      samsung/ .......... 0 differences
samsung_lego/ .... 0 differences      display-manager/ .. 0 differences
mdp/ ............ 0 differences
```

### What actually had to change (and it was far less than feared)

Only **three** core functions were missing, not a rebase:

| symbol | status in our tree | action |
|---|---|---|
| `msm_gpu_hw_init`, `msm_gpu_retire` | **already present** | none |
| `msm_gem_put_iova` | **already present** | none |
| `msm_mmu_set_fault_handler` | **already present** | none |
| `msm_clk_bulk_get` | missing | ported verbatim from v4.19 `msm_drv.c` |
| `msm_clk_bulk_get_clock` | missing | ported verbatim |
| `adreno_fw_create_bo`, `adreno_load_fw`, `adreno_request_fw` | missing | ported verbatim from v4.19 `adreno_gpu.c` |

`msm_mmu_funcs` turned out to be **already the full 15-pointer v4.19
generation**, which is precisely why the SDE coupling (five mmu callbacks) is
satisfied with no change. And `msm_gpu_funcs` was close enough that
`adreno_rbmemptrs`/`rbmemptr()` already existed on `adreno_gpu` — only the
ring-based view had to be added.

### The decisive change

`msm_drv.c` had **three** `#ifdef CONFIG_QCOM_KGSL` stubs, not one:

```c
#ifdef CONFIG_QCOM_KGSL
static void load_gpu(struct drm_device *dev) { }           /* :825  empty     */
static int add_gpu_components(...) { return 0; }          /* :2095 no-op     */
void __init adreno_register(void) { }                      /* :2274 duplicate */
void __exit adreno_unregister(void) { }
#endif
```

All three removed. That is what turns `GPU_ID` from `ENXIO` into a real
`msm_gpu`. No attach path was written — it already existed.

Note this tree has **no `CONFIG_DRM_MSM_GPU` Kconfig symbol at all**; the adreno
objects are gated purely by `ifneq ($(CONFIG_QCOM_KGSL),y)` in the Makefile.
`CONFIG_QCOM_KGSL=n` is the entire switch.

### Deviations from upstream v4.19 (all deliberate, all documented in-tree)

| deviation | reason |
|---|---|
| `active_ring` and `show_state` added as **extra** funcs members; `last_fence` and `show(gpu,seq_file*)` retained | keeps a2xx-a5xx and `msm_debugfs.c` working unmodified |
| `flush_ring` added as a **separate** member; one-arg `flush()` retained | same reason; avoids touching the SDE-adjacent call path |
| `MSM_GPU_MAX_RINGS 1` | v4.19 a6xx only ever uses `rb[0]` ("Always come up on rb 0"); the multi-ring submitqueue rework is not needed |
| `adreno_rbmemptrs` is a `#define` alias of `msm_rbmemptrs` | one allocation, two views; a `typedef` cannot satisfy `struct adreno_rbmemptrs` |
| `adreno_idle(gpu, ring)` | upstream signature; 3 legacy callers updated mechanically |
| `a6xx_opp_node_by_freq()` written locally | `dev_pm_opp_get_of_node()` does not exist in this 4.14 PM framework (added 4.16) |
| `cmd_db_get_aux_data*` names | this tree uses the CAF names, not upstream `cmd_db_read_aux_data*` |
| `of_dma_configure(dev, node)` | 2-arg form; the 3-arg form arrived in 4.16 |
| `a6xx_show()` rewritten | upstream's `adreno_show()` takes `(msm_gpu_state*, drm_printer*)`; this tree's takes `seq_file*` |
| `PC_CCU_INVALIDATE_DEPTH/COLOR` added to `enum vgt_event_type` | absent from the CAF pm4 header |
| `msm_gem_submit.seqno` added | a6xx echoes a submission counter through `CACHE_FLUSH_TS` |
| `CONFIG_QCOM_COMMAND_DB=y` | a6xx_gmu.c needs cmd-db; the driver exists but was off. Its header's `#else` branch is itself broken, so this is mandatory, not optional |

### A640 identity (milestone E) — vendor chipid left alone

The DT value was **not** changed. Instead the entry wildcards the patch id:

```c
.rev   = ADRENO_REV(6, 4, 0, ANY_ID),
.revn  = 640,
.name  = "A640",
.fw = { [ADRENO_FW_SQE] = "a630_sqe.fw", [ADRENO_FW_GMU] = "a640_gmu.bin" },
.gmem  = SZ_1M,
.init  = a6xx_gpu_init,
.zapfw = "a640_zap.mdt",
.inactive_period = 250,
```

`ANY_ID` (0xff) in the patch component means both `0x06040000` (vendor DT) and
upstream's `0x06040001` resolve to this entry, so the question of which is
"right" does not have to be answered before the driver can bind. The value read
from the hardware register will be logged at probe time and recorded before any
judgement is made. No A640 entry exists in any tree up to v5.8; this one is
authored here.

### DT change: one phandle

The hardware description was already complete — `msm_gpu@2C00000` (irq 300,
chipid, clocks, OPP, regulators, LLCC slices), `gmu@2C6A000` (irq 304/305),
`kgsl-iommu@0x02CA0000`, `pil_gpu` for ZAP. The **only** addition:

```dts
gmu = <&gmu>;      /* a6xx_gpu_init() does of_parse_phandle(node,"gmu",0)
                      and BUG_ON()s if absent. Upstream's name is plain "gmu",
                      NOT "qcom,gmu". */
```

### Config change

```diff
- CONFIG_QCOM_KGSL=y
- CONFIG_QCOM_KGSL_IOMMU=y
+ # CONFIG_QCOM_KGSL is not set
+ # CONFIG_QCOM_KGSL_IOMMU is not set
+ CONFIG_QCOM_COMMAND_DB=y
```

`CONFIG_DRM_MSM_DSI_STAGING=y` and `CONFIG_MSM_GPUCC_SM8150=y` are **retained** —
the display path is untouched. KGSL remains fully enabled in the production
kernel; only the experimental tree disables it.

### Verified in the linked `vmlinux`

```
T a6xx_gpu_init     t a6xx_submit    t a6xx_flush    t a6xx_irq
t a6xx_recover      t a6xx_destroy   T a6xx_hfi_init
T adreno_request_fw T adreno_load_fw T adreno_fw_create_bo
T msm_clk_bulk_get  T msm_clk_bulk_get_clock
4 adreno generation inits linked (a3xx, a4xx, a5xx, a6xx)
Image-dtb 48,417,628 B; sm8150.dtb + sm8150-v2.dtb + 15 overlay .dtbo built
```

### What is NOT proven, and the remaining runtime risks

The compile proves **linkage and API compatibility only**. Nothing below has
been executed on hardware.

1. **GMU firmware size — concrete blocker, found by measurement.** The Samsung
   `a640_gmu.bin` is 37,680 bytes (0x9300). `a6xx_gmu.c` enforces
   `size <= 0x8000` and would reject it. The GMU region must be enlarged or the
   check relaxed. See `GPU-FIRMWARE.md`.
2. **ZAP absent.** No `a640_zap*` exists in the stock firmware — it is
   TrustZone-provisioned. Upstream loads it via `request_firmware()` +
   `qcom_mdt_load()` + `qcom_pas_auth_and_reset()`; the device uses PIL
   (`qcom,pas-id = <13>`). Same PAS id, different path. Unresolved.
3. **IOMMU context banks.** Upstream's GMU path does
   `iommu_domain_alloc(&platform_bus_type)` + `iommu_attach_device()` on the GMU
   device, which expects a standard `arm,mmu-500` binding with an `iommus`
   property. This tree uses CAF `qcom,kgsl-smmu-v2` with per-CB child nodes
   (`gmu_user` = CB 0x4, `gmu_kernel` = CB 0x5). Reconciling the two models is
   the hardest remaining item and is **not** addressed by the compile.
4. **GMU device discovery.** `a6xx_gmu_probe()` does
   `of_find_device_by_node(node)`, which requires the GMU node to have been
   populated as a platform device. Upstream relies on arm64's default population
   of all nodes. Unverified here.
5. **Fence semantics.** Our tree retires work via `gpu->fctx`; a6xx writes a
   `seqno` and expects it echoed back. The field was added and compiles, but its
   value is never assigned and the retire path is not a6xx-aware.
6. **`BUG_ON(!node)` in `a6xx_gpu_init()`** is a hard oom if the `gmu` phandle
   is ever absent — hence the DT change is mandatory, not cosmetic.
7. **`sm_show`/debugfs and `devcoredump`** paths are compile-compatible but not
   functionally equivalent to upstream.

### Next milestone gate

Do **not** flash until M16 is designed with items 1-4 addressed. The compile-only
milestone is complete; the boot milestone is not authorised.

## RUNTIME BRING-UP PLAN

Answers the concrete question:

> **Can this Samsung kernel initialize the A640 as a real `msm_gpu`, using
> Samsung's existing hardware/power/SMMU infrastructure, without touching the
> working SDE/display path?**

**Partly — and the remaining "partly" is not a code detail.**

Two of the three issues are resolved. The third (IOMMU) is a hard architectural
blocker that must be decided before any flash.

---

### 1. A640 GMU firmware path — RESOLVED

### Which generation the transplant was using, and why it was wrong

The A6xx files came from upstream **v4.19**, which is **A630/legacy**: its GMU
firmware is one flat image copied into ITCM, capped at `0x8000`. Samsung's
`a640_gmu.bin` is 37,680 bytes (0x9330) and would have been rejected.

The A640 support and the new firmware path arrived together in **v5.8**
(November 2020) — verified by bisecting the tree:

| tag | A640 catalog entry | `struct block_header` |
|---|---|---|
| v5.4 / v5.5 / v5.6 / v5.7 | absent | absent |
| **v5.8** | **present** | **present** |

So this is exactly the split you described, and it is a single coherent commit
series rather than two loose patches.

### Firmware format verified empirically

Not assumed — parsed. `struct block_header { u32 addr, size, type, value; u32 data[] }`:

```
a640_gmu.bin  0x9330 bytes
  blk[0] @0x00000 addr=0x00000 size=0x00030 type=0xefbeadde   -> ITCM
  blk[1] @0x00040 addr=0x00200 size=0x00140 type=0xefbeadde   -> ITCM
  blk[2] @0x00190 addr=0x00400 size=0x038bc type=0xefbeadde   -> ITCM
  blk[3] @0x03a5c addr=0x04000 size=0x03a58 type=0xefbeadde   -> icache
  blk[4] @0x074c4 addr=0x40000 size=0x01b20 type=0xefbeadde   -> DTCM
  blk[5] @0x08ff4 addr=0x43fe0 size=0x00014 type=0xefbeadde   -> DTCM
  blk[6] @0x09018 addr=0x57000 size=0x00308 type=0xefbeadde   -> dcache
  7 well-formed blocks, consuming exactly 0x9330 of 0x9330 bytes
```

Every address falls inside a region the loader knows: ITCM `[0x0000,0x4000)`,
icache `[0x04000,0x40000)`, DTCM `[0x40000,0x44000)`, dcache `[0x44000,0x80000)`.
**Unambiguously block-header format.**

Contrast, same parser:

```
a630_gmu.bin  0x8000 bytes  (exactly the legacy ITCM size)
  1 block, consumes 0x19 of 0x8000   -> NOT block format
  raw ARM Thumb code: 80 3f 04 00 ...  (valid Thumb branch 0x00043f80)
a630_sqe.fw   0x7e30 bytes  0 blocks  -> SQE ucode, unrelated structure
```

### The 2025 parser fix — NOT required for this blob

v5.8 stepped blocks with dword arithmetic, `&blk->data[blk->size >> 2]`, which
truncates the advance when a size is not 4-aligned. The 2025 fix uses byte
arithmetic: `(char *)blk + sizeof(*blk) + blk->size`.

Tested both formulas against the real file:

```
a640_gmu.bin : sizes 0x30 0x140 0x38bc 0x3a58 0x1b20 0x14 0x308
               all 4-aligned: YES     dword-step == sizeof-step: YES
a630_gmu.bin : size 0x9 -> DIVERGENCE (0x18 vs 0x19)
```

**Every Samsung A640 block size is 4-aligned, so the two formulas produce
identical offsets and the 2025 fix is not needed for this firmware.** It is
nonetheless used in the port, because it is unconditionally correct and costs
nothing. (The divergence only appears in `a630_gmu.bin`, which is not
block-format at all.)

### What was changed

The legacy `0x8000` check is **untouched** and still enforced for `gmu->legacy`.
A640 simply no longer takes that path.

```c
/* a6xx_gmu.c */
struct block_header { u32 addr, size, type, value; u32 data[]; };

static bool fw_block_mem(struct a6xx_gmu_bo *bo,
                const struct block_header *blk);   /* into icache/dcache/dummy */

#define NEXT_BLK(blk) \
        ((const struct block_header *)((const char *)(blk) + \
                sizeof(*(blk)) + (blk)->size))

static int a6xx_gmu_fw_load(struct a6xx_gmu *gmu)
{
        if (gmu->legacy) {
                if (fw_image->size > 0x8000)      /* UNCHANGED legacy cap */
                        return -EINVAL;
                ... flat ITCM copy ...
                return 0;
        }
        for (blk = ...; blk < end; blk = NEXT_BLK(blk)) { ... }
}
```

Also added: `gmu_write_bulk()` (byte-offset `memcpy_toio` + one `wmb()`),
`a6xx_gmu_memory_alloc_fixed()` for buffers at a fixed IOVA, and
`adreno_is_a640()` (`revn == 640`).

### HFI version — RESOLVED

The HFI v1/v2 split hinges on the **same** `gmu->legacy` flag, so the two can
never be mixed up:

```c
if (adreno_is_a640(adreno_gpu)) {
        gmu->icache = a6xx_gmu_memory_alloc_fixed(gmu, SZ_256K - SZ_16K, 0x04000);
        gmu->dcache = a6xx_gmu_memory_alloc_fixed(gmu, SZ_256K - SZ_16K, 0x44000);
        /* gmu->legacy stays false */
} else {
        gmu->legacy = true;                 /* HFI v1, has sptprac */
        gmu->debug  = a6xx_gmu_memory_alloc(gmu, SZ_16K);
}
```

```
A618 / A630 : legacy = true  -> flat ITCM firmware, HFI v1, perf_table_v1
A640 / A650 : legacy = false -> block firmware,     HFI v2, perf_table + START
```

HFI v2 additions ported from v5.8:

* `HFI_MSG_ACK` 2 -> 1, old value preserved as `HFI_MSG_ACK_V1`
* `perf_gx_level {vote, acd, freq}` and a v2 `a6xx_hfi_msg_perf_table`
  (v1 struct renamed `a6xx_hfi_msg_perf_table_v1`)
* new messages `HFI_H2F_MSG_START 10` and `HFI_H2F_MSG_CORE_FW_START 14`
* `a6xx_hfi_start()` split into `a6xx_hfi_start_v1()` + a dispatcher

Sequence verified against v5.8 rather than guessed: v5.8 calls
`a6xx_hfi_send_gmu_init()` and `a6xx_hfi_get_fw_version()` **only** in the v1
path; the v2 path is `perf_table -> bw_table -> core_fw_start -> start`. That is
exactly what was ported.

### 2. IOMMU mapping — **NOT RESOLVED. HARD BLOCKER.**

This is the finding that stops the flash.

### What the imported code expects

Both the GPU core and the GMU allocate through a hardware SMMU:

```c
/* a6xx_gmu.c (v4.19 and v5.8) */
gmu->domain = iommu_domain_alloc(&platform_bus_type);
iommu_attach_device(gmu->domain, gmu->dev);
... iommu_map(gmu->domain, iova, phys, PAGE_SIZE, IOMMU_READ|IOMMU_WRITE);

/* v5.8 additionally: */
mmu = msm_iommu_new(gmu->dev, domain);
gmu->aspace = msm_gem_address_space_create(mmu, "gmu", 0x0, 0x80000000);
```

`iommu_domain_alloc()` needs a generic ARM SMMU instance attached to the device,
i.e. an `arm,mmu-500` node with an `iommus` property. **This device has none**,
and per your instruction none was fabricated.

### Why it cannot simply be enabled

`CONFIG_QCOM_KGSL_IOMMU` is `default y if QCOM_KGSL && (MSM_IOMMU || ARM_SMMU)`.
Setting `CONFIG_QCOM_KGSL=n` therefore **silently removed the only driver that
binds `qcom,kgsl-smmu-v2`** — verified: `kgsl_iommu.o` is absent from the
experimental build. So with KGSL off, the GPU and GMU SMMU hardware has no
driver at all.

It cannot be re-enabled standalone: `kgsl_iommu.o` belongs to `msm_kgsl_core` and
references `kgsl_driver`, `kgsl_device` and `kgsl_dt_devices`. There is no
platform_driver in it — it is a DT-match table consumed by KGSL's own device
enumeration, so it is structurally inseparable from KGSL.

### Why `msm_smmu` / `msm_mmu` is not the answer either

* `msm_smmu_new()` is called from exactly one place: `sde_kms.c:3108`.
* Its DT match table wants `qcom,smmu_sde_unsec` / `_sec` / `_nrt_*`. On this
  tree those nodes exist **only** in `sdmmagpie-sde.dtsi` (SDM845). There is no
  SM8150 node, so `msm_smmu_new()` returns an error and `sde_kms.c` `continue`s —
  **the SDE display path on this device runs with no MMU at all.**
* `msm_gpummu_new()` is *declared* in `msm_mmu.h` but has **no implementation**
  anywhere in the tree. It is a dead declaration.
* `msm_mmu_funcs` itself is already the full v4.19 generation, so the
  abstraction is fine — there is simply no SM8150 hardware backend to plug into
  it.

### The GPU genuinely cannot initialise without it

This is not a theoretical concern. `a6xx_gpu_init()` reaches
`adreno_fw_create_bo()` -> `msm_gem_kernel_new_locked()` ->
`msm_gem_get_iova(obj, aspace, iova)`. Our `msm_gpu_init()` does **not** create
an aspace, and `msm_gem_get_iova()` has **no NULL guard** on `aspace` — it
dereferences it via `lookup_vma(obj, aspace)` and
`msm_gem_get_aspace_device(aspace)`. With no address space the GPU would
fault during probe, not degrade.

### Options (each needs a decision; none is a config flip)

| # | Option | Cost | Risk |
|---|---|---|---|
| A | Extract a standalone `qcom,kgsl-smmu-v2` driver out of `kgsl_iommu.c`, exposing an `msm_mmu` | large — the driver is ~3200 lines and KGSL-shaped | medium; new driver code, but reuses proven SMMU programming |
| B | Keep `CONFIG_QCOM_KGSL=y` (so the SMMU driver builds and is programmed) while preventing KGSL from opening a client, and expose its CBs to `msm_mmu` | large | **high** — two owners of the same SMMU, and the user explicitly forbade concurrent KGSL/DRM control |
| C | Author an `arm,mmu-500` node for the GPU | small | **excluded by instruction**, and would duplicate/steal CBs from KGSL |
| D | Probe only what is reachable without an aspace (GMU mmio/irq/regulators/OPPs), deferring GEM allocation | small | gets partial evidence only; `GPU_ID` and firmware acceptance still blocked |

Option A is the only one that satisfies the stated target
(existing Samsung CAF SMMU + DRM A6xx + correct GMU CB + correct GPU CB) without
dual ownership. **Recommend A.** It is a new work item, not a continuation of
this one.

### 3. Fences / submit — DEFERRED, as instructed

`msm_gem_submit.seqno` is added and compiles, but nothing assigns it. Left
untouched on purpose. The first milestone is GPU initialisation, not submission.

---

### 4. ZAP mechanism — RESOLVED (and it is not a blocker)

Traced end to end on the running device.

* The DT node `pil_gpu` is `compatible = "qcom,pil-tz-generic"`,
  `qcom,pas-id = <13>`, `qcom,firmware-name = "a640_zap"`.
* That compatible is handled by `drivers/soc/qcom/subsys-pil-tz.c` (verified;
  `subsys-pil-tz.o` **is** built in the experimental kernel).
* On the live system `/sys/bus/platform/drivers/subsys-pil-tz/` lists
  **`soc:qcom,kgsl-hyp`** — the node is bound and PAS 13 is being driven.
* KGSL never loads a ZAP image. Its A640 catalog entry only records the *name*:
  ```c
  .sqefw_name = "a630_sqe.fw",
  .gpmufw_name = "a640_gmu.bin",
  .zap_name   = "a640_zap",
  .core = 6, .major = 4, .minor = 0, .patchid = 0,   /* chipid 0x06040000 */
  .gmem_size = SZ_1M,
  .gpmu_major = 0x2, .gpmu_minor = 0x000,
  ```
  and `adreno_a6xx.c:998` does `subsystem_get(adreno_dev->gpucore->zap_name)` —
  a **handle lookup**, not a firmware load. `subsys-pil-tz` authenticates and
  starts the PAS through TrustZone (`pil_ops_trusted` + SCM).
* **The transplanted code never loads a ZAP.** Upstream v4.19 has zero
  `zap` references in `adreno_gpu.c` and our `a6xx_gpu.c` has zero as well;
  `zapfw` is only a field in the catalog entry. (The
  `request_firmware()` + `qcom_mdt_load()` + `qcom_pas_auth_and_reset()` path
  is later upstream work that is simply not present here.)

**Conclusion:** on this device the ZAP is provisioned and started by
TrustZone/PIL before the DRM driver runs, so there is nothing for the DRM path
to load and no missing-file failure to encounter. The absence of any
`a640_zap` blob in the firmware package is therefore **not** a blocker, and no
fake image is needed.

### Bonus: the KGSL entry independently confirms two earlier decisions

```
KGSL .core=6 .major=4 .minor=0 .patchid=0   ->  0x06040000
upstream ADRENO_REV(6, 4, 0, ANY_ID)
```

The vendor chip ID is genuinely `0x06040000`, and upstream itself wildcards the
patch component with `ANY_ID`. Our `ADRENO_REV(6, 4, 0, ANY_ID)` entry matches
upstream exactly and needs no DT change. (The `0x06040001` figure that appeared
in an earlier draft of this document is **not** an upstream chip ID and is
withdrawn.)

---

### Required DT changes

| change | status |
|---|---|
| `gmu = <&gmu>;` on `msm_gpu@2C00000` | **done** — mandatory; `a6xx_gpu_init()` `BUG_ON()`s without it |
| GPU `iommus` / SMMU node | **NOT DONE — blocked on the IOMMU decision** |
| GMU `iommus` | **NOT DONE — blocked on the IOMMU decision** |
| `qcom,chipid` | **unchanged** (0x06040000), correct as-is |
| everything else (IRQ 300/304/305, OPP, regulators, clocks, LLCC) | already present and sufficient |

### Required kernel config

```diff
- CONFIG_QCOM_KGSL=y
- CONFIG_QCOM_KGSL_IOMMU=y
+ # CONFIG_QCOM_KGSL is not set
+ # CONFIG_QCOM_KGSL_IOMMU is not set
+ CONFIG_QCOM_COMMAND_DB=y          (needed by a6xx_gmu.c; its header's #else
                                     branch is itself broken, so mandatory)
  CONFIG_DRM_MSM_DSI_STAGING=y      (unchanged - display path)
  CONFIG_MSM_GPUCC_SM8150=y         (unchanged - SM8150 GPUCC)
```

### Expected probe sequence (once IOMMU is solved)

```
adreno_load_gpu()          -> adreno_info(rev 6.4.0.x) -> "A640" matched
adreno_load_fw()           -> qcom/a630_sqe.fw, qcom/a640_gmu.bin accepted
adreno_gpu_init()          -> ring 0 created, memptrs wired
a6xx_gmu_probe()           -> gmux mmio @0x2c6a000, IRQs 304/305, regulator vdd,
                              clocks, (icache@0x04000, dcache@0x44000 for A640)
a6xx_gmu_memory_probe()    -> SMMU domain attached        <-- BLOCKED TODAY
a6xx_gmu_fw_start()        -> a6xx_gmu_fw_load() walks 7 blocks into
                              ITCM/DTCM/icache/dcache, legacy cap NOT applied
a6xx_hfi_init()            -> HFI v2 (gmu->legacy == false)
a6xx_hw_init()             -> SQE ucode in sqe_bo
a6xx_cp_init()             -> CP_ME_INIT on rb[0]
priv->gpu set              -> GPU_ID ioctl returns a chip id, not ENXIO
```

### First runtime success criteria

Kernel-level only. No Mesa, no Weston GL, no kmscube, no 3D workload.

1. `dmesg` shows `Found GPU: 6.4.0.x` and the A640 entry selected
2. GMU mmio mapped, HFI IRQ 304 and GMU IRQ 305 registered
3. `a640_gmu.bin` accepted: **7 blocks logged, 0 "failed to match fw block"**
   (a direct, quantitative check that the block parser and the i/d cache
   IOVAs are right)
4. HFI v2 handshake completes; no `HFI_MSG_ACK` mismatch
5. SMMU domain attached for both GPU and GMU, with CBs not in use by anything else
6. `GPU_ID` ioctl returns a real chip ID — `ENXIO` is gone

Only after all six: ringbuffer / seqno / fence retire / command submission.

### Experimental build artifacts (built and hashed, NOT flashed)

```
Image       477e03e3cf011a61aa8b265055194511d5ef08c021f61be93a811377e0de8bc0
Image-dtb   b8a6caea9de9f2e4c9db2bd2c624597ab9b78e2a70c0dbfcbae1b929d8df3c46
sm8150.dtb  f933838ab991895daa48ee976e8bc2525b63b923dc576ef840155c76d57c71a7
.config     c77676e455cc2ebb36d373a25d435a5e7eb0d960b3fd8012f73f9a1325cb6621
```

Diffstat vs the production tree:

```
drivers/gpu/drm/msm/   24 files changed, 8685 insertions(+), 85 deletions(-)
                       (7753 of the insertions are the verbatim a6xx files)
arch/.../sm8150-gpu.dtsi   1 file changed, 14 insertions(+)

sde/ 0   dsi/ 0   dsi-staging/ 0   samsung/ 0   samsung_lego/ 0
display-manager/ 0   mdp/ 0
```

Build passes with **0 errors and 0 warnings** after the A640 port. Both
firmware paths are present in the linked image, verified by their strings:

```
"GMU firmware is bigger than the available region"   <- legacy 0x8000 cap, retained
"failed to match fw block (addr=%.8x size=%d)"       <- A640 block path, new
```

and the compiled DTB carries the mandatory phandle:

```
qcom,kgsl-3d0@2C00000 { ... gmu = <0x6b>; }
/aliases { gmu = "/soc/qcom,gmu@0x2C6A000"; }
```

Production tree verified untouched: no files modified, `CONFIG_QCOM_KGSL=y`
and `CONFIG_QCOM_KGSL_IOMMU=y` still set, recovery image 62 intact
(52,662,272 bytes).

### Flash gate — CURRENTLY CLOSED

Your preconditions were:

* A640 GMU path is coherent — **YES**
* IOMMU strategy is understood — **NO, blocker**
* ZAP mechanism is understood — **YES**
* experimental image built and hashed — **YES** (below)

Because the IOMMU precondition is not met, **do not flash.** A boot now would
almost certainly fault in `msm_gem_get_iova()` with a NULL address space, which
would produce a failed runtime state and no usable logs.

## IOMMU ADAPTER

Answers the task's narrow question:

> Make the existing Qualcomm CAF SMMU/context-bank infrastructure usable by the
> transplanted DRM A6xx driver without reintroducing KGSL GPU ownership and
> without touching the working Samsung SDE.

**Done, source-complete and compile-tested.** One new 268-line adapter file,
plus four small edits. Nothing from KGSL is re-enabled and no SMMU register
programming was reimplemented.

### The finding that made this small

The GPU SMMU is **not** driven by KGSL. Verified on the live device:

```
/sys/bus/platform/devices/2ca0000.kgsl-smmu -> arm-smmu
```

The DT SMMU node is:

```dts
kgsl-smmu@0x02ca0000 {
        compatible = "qcom,qsmmu-v500";
        #iommu-cells = <2>;
        reg = <0x02ca0000 0x10000>, <0x02cc0000 0x200000>;
};
```

and `qcom,qsmmu-v500` is matched by **the generic `drivers/iommu/arm-smmu.c`**
(`{ .compatible = "qcom,qsmmu-v500", .data = &qcom_smmuv500 }`), which CAF
extended. It is built (`CONFIG_ARM_SMMU=y`, `arm-smmu.o` present in the
experimental build) and entirely independent of `CONFIG_QCOM_KGSL`.

The context banks are ordinary DT children carrying standard `iommus`, so the
**generic** IOMMU API can consume them. Read from the live device tree:

```
qcom,kgsl-iommu@0x02CA0000 (compatible "qcom,kgsl-smmu-v2")
  gfx3d_user     iommus = <0x24b 0x0 0x401>     CB 0, SID 0x401
  gfx3d_secure   iommus = <0x24b 0x2 0x400>     CB 2, SID 0x400
qcom,gmu@0x2C6A000
  gmu_user       iommus = <0x24b 0x4 0x400>     CB 4, SID 0x400
  gmu_kernel     iommus = <0x24b 0x5 0x400>     CB 5, SID 0x400
```

The one genuine gap: those CB **platform devices are created by
`kgsl_iommu.c`'s own `of_platform_populate()`**. `of_default_bus_match_table`
in this tree contains only `simple-bus`/`simple-mfd`/`isa` and **no**
`of_match_default`, so with `KGSL=n` nothing creates them. That, and only
that, is what the adapter supplies.

### Context-bank topology and selection

| consumer | CB node | CB | SID | needed for bring-up |
|---|---|---|---|---|
| **GPU core (a6xx)** | `gfx3d_user` | 0 | 0x401 | **yes** |
| **GMU kernel buffers** | `gmu_kernel` | 5 | 0x400 | **yes** |
| GMU process pages | `gmu_user` | 4 | 0x400 | no (userspace) |
| GPU secure buffers | `gfx3d_secure` | 2 | 0x400 | no (secure allocs) |

The GMU choice is traced, not assumed: `kgsl_gmu.c` maps GMU kernel memory
(HFI queues, A640 icache/dcache, debug) through
`alloc_and_map(gmu, GMU_CONTEXT_KERNEL, ...)` where
`GMU_CONTEXT_KERNEL` maps to `"gmu_kernel"`.

GPU and GMU deliberately get **separate domains and separate devices**. They are
not collapsed: the GPU core addresses memory through `gfx3d_user`, while the
GMU's own buffers are mapped with `iommu_map()` on `gmu_kernel`.

### Exact objects and mechanism

```c
/* reference sequence, taken from CAF's own gmu_iommu_cb_probe() */
pdev    = of_find_device_by_node(cb_node);          /* CB as platform dev */
of_dma_configure(&pdev->dev, cb_node);
domain  = iommu_domain_alloc(&platform_bus_type);
ret     = iommu_attach_device(domain, &pdev->dev);
mmu     = msm_iommu_new(&pdev->dev, domain);        /* existing wrapper  */
aspace  = msm_gem_address_space_create(&pdev->dev, domain, name);
```

| step | API used | present in this tree |
|---|---|---|
| populate CB | `of_platform_populate(parent, NULL, NULL, &parent->dev)` | yes (idempotent, `OF_POPULATED`) |
| find CB device | `of_find_device_by_node()` | yes |
| DMA config | `of_dma_configure(dev, node)` | yes |
| domain alloc | `iommu_domain_alloc(&platform_bus_type)` | yes |
| attach | `iommu_attach_device(domain, dev)` | yes |
| wrap | `msm_iommu_new(dev, domain)` | **already in `msm_iommu.c`** |
| aspace | `msm_gem_address_space_create(dev, domain, name)` | CAF 3-arg form |
| map / unmap | `msm_iommu_map()` -> `iommu_map_sg()`, `msm_iommu_unmap()` -> `iommu_unmap()` | existing, **untouched** |
| fault handler | `iommu_set_fault_handler()` inside `msm_iommu_new()` | existing |

**No new map/unmap code was written.** `msm_iommu.c` already provides the whole
15-callback generation and is byte-identical to the production file.

### Generation differences found and adapted (this is a pre-DMA-API IOMMU)

The kernel's IOMMU core is older than what modern upstream code assumes:

| modern API | this tree | adaptation |
|---|---|---|
| `iommu_domain_get_id()` | does not exist | log `domain->geometry.aperture_start/end` instead |
| `struct iommu_domain` has no `.id` | — | no CB-number assertion is possible; logged from DT instead |
| `msm_gem_address_space_create(mmu, name, start, end)` | CAF `(dev, domain, name)` | used the CAF form; IOVA window comes from the domain aperture |
| `for_each_of_node()` | absent | used `for_each_of_allnodes()` (DT-only walk, which is what is wanted) |
| `platform_device_alloc_from_of_node()` | absent | used `of_platform_device_create(np, NULL, NULL)` |

Copying a modern `msm_iommu.c` wholesale would have failed on all five. None of
these are workarounds; they are the actual APIs of this kernel.

### The adapter

`drivers/gpu/drm/msm/msm_gpu_mmu.c`, 268 lines (roughly 120 excluding comments).
Two entry points:

```c
struct msm_gem_address_space *msm_gpu_aspace_new(const char *cb_name,
                                                const char *aspace_name);
int   msm_gpu_cb_domain_get(const char *cb_name, struct iommu_domain **domain,
                            struct device **dev);
void  msm_gpu_cb_domain_put(struct iommu_domain *domain, struct device *dev);
```

Call sites (the only two):

```c
/* a6xx_gpu.c: before anything allocates GPU memory */
aspace = msm_gpu_aspace_new("gfx3d_user", "gfx3d");
if (IS_ERR(aspace)) { ...fail cleanly... }
gpu->aspace = aspace;

/* a6xx_gmu.c: replaces iommu_domain_alloc()+iommu_attach_device() */
ret = msm_gpu_cb_domain_get("gmu_kernel", &gmu->domain, &gmu->cb_dev);
gmu->dev = gmu->cb_dev;
```

Domain ownership is explicit: the address space owns the domain through its
`msm_mmu` (`msm_iommu_destroy()` calls `iommu_domain_free()`), so the adapter
does not free it. The GMU path takes raw ownership and releases it in
`a6xx_gmu_remove()` via `msm_gpu_cb_domain_put()`, unconditionally, so a
mid-probe failure cannot leak the domain.

### KGSL ownership status

**No KGSL GPU ownership.**

```
CONFIG_QCOM_KGSL            = not set
CONFIG_QCOM_KGSL_IOMMU      = not set
kgsl_iommu.o / kgsl.o       = not built
/dev/kgsl-3d0               = will not exist in this image
```

The adapter deliberately reuses only the *generic* ARM SMMU driver
(`arm-smmu.c`, `CONFIG_ARM_SMMU=y`) plus the existing `msm_iommu.c` wrapper.
No `drivers/gpu/msm/` file is compiled, so KGSL cannot probe, cannot open the
A640 registers, and cannot claim the context banks.

### Instrumentation (for the first boot)

`MSM_GPU_MMU_DEBUG` (default 1) emits, per context bank, enough to distinguish
every failure mode called out in the task:

```
found GPU address space ready on gfx3d_user
cb 'gfx3d_user' (...) attached, domain=... aperture=0x...-0x...
aspace 'gfx3d' on cb 'gfx3d_user': iova 0x...-0x...
GMU using context bank gmu_kernel, domain=...

# error paths, distinct messages:
failed to obtain GMU context bank 'gmu_kernel': <rc>   -> CB node/device problem
context bank '<name>' did not become a device            -> population problem
iommu_domain_alloc failed for '<name>'                   -> no SMMU / domain
iommu_attach_device failed for '<name>': <rc>            -> CB/SID mismatch
failed to create GPU address space on gfx3d_user: <rc>   -> wrapper problem
```

That maps 1:1 onto the requested distinction between *domain unavailable*,
*device not populated*, *attach failure*, *context-bank mismatch* and *map
failure* (the last surfaces from `msm_iommu_map()`'s existing
`WARN_ON(!ret)`).

### Diffstat

```
drivers/gpu/drm/msm/   26 files changed, 9043 insertions(+), 85 deletions(-)
                       (7753 of the insertions are the verbatim a6xx files;
                        269 of them are the new msm_gpu_mmu.c)
arch/.../sm8150-gpu.dtsi    1 file changed, 14 insertions(+)

This task's contribution:
  msm_gpu_mmu.c   NEW  +269      (the adapter)
  msm_mmu.h             +26      (2 declarations, 1 include, 1 debug macro)
  a6xx_gpu.c             +22      (GPU aspace creation)
  a6xx_gmu.c             ~+35/-20 (GMU CB domain + teardown)
  Makefile               +1
```

**SDE diffstat: zero. Verified:**

```
sde/ 0    dsi/ 0    dsi-staging/ 0    samsung/ 0    samsung_lego/ 0
display-manager/ 0    mdp/ 0    hdmi/ 0    dp/ 0
```

And the existing MMU core is untouched:

```
msm_gem_vma.c  IDENTICAL
msm_iommu.c    IDENTICAL
msm_smmu.c     IDENTICAL
```

Only `msm_mmu.h` changed, and only to add declarations plus a
`dma-mapping.h` include.

### First-flash gate

```
GPU context-bank:        gfx3d_user  (CB 0, SID 0x401)
GMU context-bank:        gmu_kernel  (CB 5, SID 0x400)
IOMMU domain mechanism:  iommu_domain_alloc(&platform_bus_type)
exact device used:       the CB platform device (of_find_device_by_node)
attach mechanism:        iommu_attach_device(domain, &cb_dev)
map/unmap mechanism:     existing msm_iommu_map/unmap -> iommu_map_sg/iommu_unmap
                         GMU buffers use iommu_map() directly on its own domain
KGSL ownership status:   NONE - KGSL and KGSL_IOMMU both off, no drivers/gpu/msm
                         objects built, /dev/kgsl-3d0 absent
display/SDE changes:     NONE - byte-for-byte identical
expected first failure:  a6xx_hw_init() after the GMU is up, or the HFI v2
                         handshake, if the ZAP/firmware sequence differs from
                         KGSL's. GPU init itself should now complete.
```

## VERDICT

A coherent donor layer exists and **is separable without rebasing SDE**. Per task
section 13 this clears the bar to proceed to a build-only experiment.

The decisive discovery is that the DRM GPU attach path is already in the tree and
gated only by `#ifdef CONFIG_QCOM_KGSL`, and that our `msm_mmu_funcs` already
matches the v4.19 generation that SDE depends on.

WORKING: the resource match, the attach path, the SDE isolation argument.
BROKEN: nothing.
NOT TESTED: compile, IOMMU CB assignment, ZAP load, runtime GPU bring-up.
FUTURE: Mesa/Freedreno, Weston GL.