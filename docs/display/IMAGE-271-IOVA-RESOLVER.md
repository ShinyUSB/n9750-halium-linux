# Image 271 — resolve a dangling IOVA to the BO that owns it

Date: 2026-10-18
Base: **image 270** (which itself is 263 + the page-table prologue). So the chain is
263 → 270 → 271, each step changing exactly one thing.
Variable changed vs 270: **one** — the kernel.

## Artifact

```
/home/usb/artifacts/flash/271-halium-boot-a6xx-iovaresolve.img
sha256  6730f7c712e92b4a208b75c9bbd123b7056372a2e4863b4b6ea32649f9960722
kernel  sha256 3f73667884754b92bd37d0b2eeb2f11758fcf5f05748eb93d8ee209a8bfd2b59
        (= freshly built out-gpu/.../Image-dtb, byte-identical)
ramdisk 4117936 bytes, sha eb5d2eb5… (ramdisk242-good.bin, verified by the packer)
```

## What it changes

One new static function plus one call site, in
`drivers/gpu/drm/msm/adreno/a6xx_gpu.c`. 104 lines added, 2 removed. No other file
touched; no register written; no allocation; no submit-path change.

### The resolver

```c
static void a6xx_resolve_dangling_iova(struct msm_gpu *gpu, uint64_t iova,
		unsigned int bo_idx, unsigned int dword)
```

It walks the GPU address space's active list and finds the BO whose VMA contains
the IOVA, then prints that BO's identity:

```
PTU271[resolve] iova 0x700027000 (bo[3] dword 36) OWNED BY bo:
    iova=0x700027000 size=0x1000 mmoff=0x102179000 objsz=4096
    boflags=0x20000 name='' off=0x0
```

or, if no BO owns it:

```
PTU271[resolve] iova 0x... (bo[3] dword 36) NOT OWNED by any BO in the
    address space; nearest lower vma iova=0x... size=0x...
```

### Hook point

The image-260 DANGLING IOVA scan, which already runs inside `if (ibdump)`:

```c
if (!ok) {
        pr_info("BOS[seq=%u] DANGLING: bo[%u] dword %u -> iova 0x%llx ...");
        a6xx_resolve_dangling_iova(gpu, v, b, j);
}
```

`ibdump` is a **runtime** module parameter (`/sys/module/msm_drm/parameters/ibdump`),
so the instrument can be switched on and off without a flash.

## Why the owning BO is findable

freedreno obtains every BO's GPU address through the `MSM_INFO_GET_IOVA` ioctl
(`src/freedreno/drm/msm/msm_bo.c:113`, `msm_bo_iova()`). The kernel's handler
calls `msm_gem_get_iova()`, which:

- inserts a VMA into `aspace->mm` and sets `vma->iova = vma->node.start << PAGE_SHIFT`
  (`msm_gem_vma.c:227,233`),
- maps it into the MMU (`msm_gem_map_vma()`),
- adds the object to `aspace->active_list` (`msm_gem.c:703-707`).

The shader ISA BO must have gone through exactly this path, because the a6xx
hardpin path writes `bo->iova` into the command stream — an IOVA that the BO
obtained via that ioctl.

The resolver therefore looks at:

- `aspace->active_list` (linked through `msm_gem_object::iova_list`),
- each object's `vmas` list (each `msm_gem_vma` has `iova`, `node.size` in pages,
  and `aspace`),
- and reports the owner's `msm_gem_mmap_offset()` — the DRM fake offset of the
  BO's mmap.

**That last number is the point.** It is exactly the *file offset* the userspace
`vmascan8.py` reports for a `/dev/dri` mapping. So for the first time the kernel
view and the userspace view of the same buffer can be joined by name rather than
by size or guesswork.

## The decision this buys

The hypothesis under test: *the dangling IOVA in the a6xx shader descriptor is
the shader program's IOVA.*

| outcome | meaning |
|---|---|
| resolves to a BO with mmoff **0x102179000** (or 0x10217a000 / 0x10217b000) | **proven.** The draw points at the shader ISA, the ISA BO is not submitted, and the root cause is closed. |
| resolves to some other BO | the mechanism is different — and that is equally worth knowing. |
| `NOT OWNED by any BO` | the IOVA was never allocated at all; the descriptor references a stale/garbage address, which points at a different class of bug. |

## Verification performed

```
[PASS] 271 kernel payload == freshly built Image-dtb
[PASS] 271 ramdisk == ramdisk242-good.bin
[PASS] 271 kernel differs from 270's kernel
[PASS] 271 ramdisk identical to 270's ramdisk
[PASS] cmdline identical to 270 (single variable = kernel only)
[PASS] header diffs confined to the id field 576-595
[PASS] swiotlb=2048 present; cmdline not truncated beyond "a6xx.send_ac"
[PASS] PTU271 present; PTU270 present (base preserved)
[PASS] no 269 submitqueue-query ioctl
```

The patch was also verified to apply *forward* to a reconstructed 270 baseline and
reproduce the built tree byte-for-byte.

## Falsifiability

The resolver prints one line per dangling IOVA. If the DANGLING scan does not
fire at all, the test is void (the instrument is not running) and nothing can be
concluded. Expect both `BOS[seq=…] DANGLING:` and `PTU271[resolve]` lines in
dmesg; if only the first appears, the resolver did not run.

## Patch

`/home/usb/artifacts/gpu-bringup/patches/271-resolve-dangling-iova.patch`
