# Image 270 — CP_SMMU_TABLE_UPDATE page-table prologue

Date: 2026-10-18
Base: **image 263** (deliberately, not 269 — see "Why 263" below)
Variable changed: **one** — the kernel. Nothing else in the image differs.

## Artifact

```
/home/usb/artifacts/flash/270-halium-boot-a6xx-ptprologue.img
sha256  d4085673e4bdaeec3de126e5bc6f9f9b9e88480ae8fdabcd3fdffcaa1831e726
size    52543488 bytes
kernel  48418114 bytes  (= freshly built out-gpu/.../Image-dtb, byte-identical)
ramdisk 4117936 bytes   (= ramdisk242-good.bin, sha eb5d2eb5… verified)
```

## The hypothesis

Upstream `a6xx_submit()` emits a per-context page-table prologue that this tree
emits **nowhere**:

```c
static void a6xx_set_pagetable(struct msm_gpu *gpu, struct msm_gem_submit *submit)
{
	struct msm_mmu *mmu = submit->queue->ctx->vm->mmu;
	uint64_t ttbr;
	int asid;

	if (mmu->funcs->get_pagetable_params(mmu, &ttbr, &asid))
		return;                     /* no page table yet — skip, do not fail */

	/* Wait for any writes in flight to land */
	a6xx_wait_for_idle(gpu);
	OUT_PKT4(ring, CP_SMMU_TABLE_UPDATE,
		TTBR0_LO(ttbr),
		TTBR0_HI(ttbr) | ASID(asid),
		CONTEXTIDR(0),
		CONTEXTBANK(0));
}
```

This tree already noticed the omission itself — image 209's diagnostic comment at
`a6xx_gpu.c:1598-1601` says *"upstream's a6xx_submit() emits a prologue we emit
nowhere: … CP_SMMU_TABLE_UPDATE with TTBR0+ASID"*. The corpus later retired the
hypothesis on the premise that the packet is **v5.10+ only**. That premise is
wrong:

- PM4 spec (`regs/mesa-pm4.xml`): `<value name="CP_SMMU_TABLE_UPDATE" value="0x53"
  variants="A5XX-"/>` — the comment above it reads *"switches SMMU pagetable, used
  on a5xx+ only"*. A6xx is in range.
- The vendor KGSL driver on this silicon class emits it at `adreno_iommu.c:590`,
  commented *"CP switches the pagetable and flushes the Caches"*.

**Why it would explain every observation.** The defect is shared between the RB
(render) and SP (compute) write paths — T1 (compute→SSBO) and T2 (transform
feedback→TBO) are both dead, and Mesa's state is demonstrably live and
workload-tracking in the aperture registers (image 267). TTBR0 is the base the
shader/interconnect domain translates through. With no page table installed,
shader-domain stores have no valid translation — but the fault is silent: no SMMU
fault is ever raised, the CP still writes DDR (`memptrs->fence` increments), and
nothing from any shader ever reaches memory. That is exactly the signature.

## What the patch does

Three files, ~99 added lines, all additive:

| File | Change |
|---|---|
| `msm_mmu.h` | new `get_pagetable_params` entry in `struct msm_mmu_funcs` |
| `msm_iommu.c` | implements it via `iommu_domain_get_attr(domain, DOMAIN_ATTR_TTBR0, &v)` |
| `adreno/a6xx_gpu.c` | calls it in `a6xx_submit()` immediately before the cmd loop and emits the packet |

Emission point is `a6xx_gpu.c:1364+`, after the CCU invalidates and **before**
`for (i = 0; i < submit->nr_cmds; i++)` — the same position upstream uses. The
cmd loop itself is untouched.

### Field packing

Our SMMU already folds the ASID into `DOMAIN_ATTR_TTBR0`
(`drivers/iommu/arm-smmu.c:3627`):

```c
val  = pgtbl_cfg.arm_lpae_s1_cfg.ttbr[0];
val |= (u64)asid << TTBRn_ASID_SHIFT;      /* that shift is 48 */
```

The packet wants `TTBR0_LO = bits[31:0]` and `TTBR0_HI = bits[63:32]`, so
emitting the two halves of the raw value puts the ASID in the right place
automatically. Upstream masks and re-shifts because it stores ttbr and asid
separately; here that is unnecessary **and** it avoids hardcoding
`TTBRn_ASID_SHIFT`, which lives in `drivers/iommu/arm-smmu-regs.h:235`, a private
IOMMU header `msm` cannot include.

### Deliberate omissions

- **`CP_MEM_WRITE` of TTBR0 to the memstore** — upstream's "for debugging /
  preemption". Our memptrs layout has no PT_INFO slot. Skipped.
- **`CP_SET_PROTECTED_MODE`** — upstream emits it as part of the prologue; our tree
  never enables protected mode, so there is nothing to turn off.
- **Failure-on-missing-page-table** — `get_pagetable_params()` returns `-EINVAL`
  when the domain has no page table yet (what arm-smmu reports pre-attach). The
  caller **skips** and lets the submit proceed. A missing prologue must never fail
  a submit.
- **Guard against per-submit spam** — the packet is emitted on every submit, not
  only on context change. Deliberate: this tree has no per-context VM, so
  `submit->queue->ctx->vm` does not exist (see below) and `gpu->aspace` is the
  only available path. It is correct for every submit, merely not minimal.

### Known divergence: `gpu->aspace` vs `submit->queue->ctx->vm`

This 4.14 CAF tree does not have upstream's submit structure. `a6xx_submit()`
here is `(gpu, submit, ctx)` where `ctx` is a `struct msm_file_private` holding
only `enable_refcnt` / `power_lock` — no VM, no address space. The tree's own
instrumentation already reaches the MMU this way (`a6xx_gpu.c:2712` logs
`aspace=%px mmu=%px` from GPU code), so `gpu->aspace->mmu` is the established
path. If per-context page tables are ever needed, this becomes the bug to fix.

## Why 263 and not 269

Base is image 263 so the variable count stays at one. 269 adds two unrelated
changes (`MSM_VERSION_MINOR` 4→6 and the `MSM_SUBMITQUEUE_QUERY` ioctl stub) —
if the test renders a non-black pixel we would not know whether the prologue or
the version bump did it. 269 can be re-applied on top afterwards; it is a
two-file change (`msm_drv.c`, `include/uapi/drm/msm_drm.h`) and it is already
written and proven good.

## Verification performed

```
[PASS] 270 kernel payload == freshly built Image-dtb
[PASS] 270 ramdisk payload == ramdisk242-good.bin (sha eb5d2eb5…)
[PASS] 270 kernel differs from 263's kernel
[PASS] 270 ramdisk identical to 263's ramdisk
[PASS] cmdline identical to 263 (single variable = kernel only)
[PASS] all header diffs confined to id field 576–607
[PASS] cmdline not truncated beyond "a6xx.send_ac"
[PASS] swiotlb=2048 present
[PASS] no a6xx.uche_gbif_rw (263 baseline, not 268)
```

Tree state was first reverted to exact image-263 content (reverse-applied the 267
aperture patch; `MSM_VERSION_MINOR` back to 4; uapi header restored) before the
prologue was applied. Compile oracle is kbuild itself; two earlier failures were
real (`CP_SMMU_TABLE_UPDATE_1_ASID_SHIFT` undefined in this tree's trimmed
`adreno_pm4.xml.h`, and a nested `/* */` inside a block comment).

## Falsifiability

The patch prints one line per submit:

```
PTU270[seq=N] ttbr0=0x… asid=…
PTU270[seq=N] no pagetable params, prologue skipped
```

A negative result is only meaningful if the **first** line appears with a
plausible TTBR0. If the second line appears, the test is void — the driver never
consumed the value and the null result would mean nothing. Verify against serial
and/or ramoops, not only a filesystem log.

## Patch

`/home/usb/artifacts/gpu-bringup/patches/270-pagetable-prologue.patch`