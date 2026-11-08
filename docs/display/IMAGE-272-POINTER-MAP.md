# Image 272 — POINTER MAP: every GPU address in the command stream, and what it points at

Date: 2026-10-18
Base: **image 271** (which is 270 + the PTU271 resolver). So 263 → 270 → 271 → 272,
each step changing exactly one thing.
Variable changed vs 271: **one** — the kernel.

## Artifact

```
/home/usb/artifacts/flash/272-halium-boot-a6xx-ptrmap.img
sha256  927734cc1cb28d30ae036aa0244a8aaa65304da21ca1aba191bb50cfed2153b5
kernel  sha256 47a0ea6c3da59b22e98722d8dda90f71c030c3c2e93d25d1a7afc30b3f5f8fd8
        (= freshly built out-gpu/.../Image-dtb, byte-identical)
```

## What it changes

The image-260 DANGLING IOVA scan is **replaced** by a pointer map. One file,
`drivers/gpu/drm/msm/adreno/a6xx_gpu.c`; 141 lines added, 121 removed.

### Why the DANGLING scan had to go

Image 271's resolver proved it was wrong in two independent ways:

1. **It used exact IOVA equality instead of containment**, so every *interior*
   pointer into a submitted buffer was reported as unmapped. Measured:

   | reported "dangling" | actually |
   |---|---|
   | `0x700027000` | offset **0x1000 inside** the submitted 20480-byte BO |
   | `0x700021010` | offset **0x10 inside** the submitted 4096-byte BO |
   | `0x700000007` | offset 0x7 inside a submitted BO |

2. **It only reported the addresses that did NOT resolve.** It was completely
   silent about the pointers that do — which is where the answer is.

This instrument was already on the project's list of eleven that produced
confident wrong answers ("the `DANGLING` scan (exact IOVA equality instead of
containment)", `STATE-END-OF-SESSION-2026-11-16.md`). I rebuilt on it anyway;
the resolver is what caught it.

### The pointer map

For **every** 64-bit value in the aperture range in every submitted buffer, the
driver now prints which buffer the value lands in, by containment, with the
offset within it:

```
PTRMAP[seq=4] bo[3] dword 36 -> iova 0x700027000 -> BO mmoff=0x102189000 base=0x700026000 size=0x5000 objsz=20480 boflags=0x20000 name='' off=0x1000
PTRMAP[seq=4] bo[3] dword 52 -> iova 0x70002a000 -> BO mmoff=0x102179000 base=0x70002a000 size=0x1000 objsz=4096 boflags=0x20000 name='' off=0x0
PTRMAP[seq=4] bo[3] dword 60 -> iova 0x70002b000 -> NO BO OWNS THIS IOVA (nearest lower vma iova=0x70002a000 size=0x1000)
```

The `mmoff` is the DRM fake offset of the owning BO's mmap — **exactly** the
file offset that the userspace `vmascan8.py` reports for a `/dev/dri` mapping.
That is the join between the kernel's view and the userspace's view of the same
buffer, and it is what makes the two comparable at all.

### Alignment

Pointers are scanned at **every** dword boundary, not just even ones — a pointer
need not be even-aligned, and the image-271 evidence only covered the even
case. False positives are essentially impossible: the high half of any pointer
in this aperture is exactly `0x00000007`, so only dwords equal to 7 can produce
a hit.

### Safety

The resolver machinery is the one proven in image 271:

- enumerates `gpu->active_list` and `priv->inactive_list` (both linked through
  `msm_gem_object::mm_list`), **not** `aspace->active_list`, which is
  `{NULL,NULL}` on this tree and oopses the kernel (image 271's oops);
- takes **no** `aspace->list_lock`, which is likewise never `mutex_init`'d on
  this path;
- relies on `dev->struct_mutex`, which the submit path already holds.

## What the dump will settle

One PTRMAP dump of the a6xx shader descriptor answers the open question, and
the three possible outcomes mean three different things:

| outcome | meaning |
|---|---|
| pointer lands in the **ISA buffer** `mmoff=0x102179000` | the shader program *is* carried by the submission; the "ISA missing" line of reasoning collapses and the fault is downstream of it |
| pointer lands in a buffer **not in the submission** | the program is genuinely not carried; the mechanism is freedreno's, and the 20480-byte buffer is something else (most likely `pvtmem_per_fiber`, see below) |
| pointer lands in the **all-zero** buffer `mmoff=0x102189000` | the program the GPU is told to run is empty, and the fault is the upload |

**Reading (b) is currently the most likely.** The 20480-byte buffer that the
descriptor's dword 36 points into is `FD_BO_NOMAP` (it has no userspace
mapping, as observed) and its size matches freedreno's private-memory buffer
exactly: `fd6_program.c:117-120` creates `ctx->pvtmem[...].bo` as
`fd_bo_new(dev, total_size, FD_BO_NOMAP, "pvtmem_%s_%d", ...)` with
`total_size = ALIGN(per_fiber_size, 1<<12) * num_sp_cores`; a640 has 4 SP cores
and 20480/4 = 5120 = `per_fiber_size`. If so, dword 36 is the **private-memory
address, not the program address**, and the real ISA pointer is at a dword the
broken scan never reported. The pointer map finds it.

Also still open, and answerable in the same dump: whether the three
magic-bearing ISA buffers (userspace mmoffs `0x102179000` FS, `0x10217a000` and
`0x10217b000` VS) are referenced by the stream at all.

## Verification performed

```
[PASS] 272 kernel payload == freshly built Image-dtb
[PASS] 272 ramdisk == ramdisk242-good.bin, identical to 271's
[PASS] 272 kernel differs from 271's kernel
[PASS] cmdline identical to 271 (single variable = kernel only)
[PASS] header diffs confined to the id field 576-595
[PASS] swiotlb=2048 present; cmdline not truncated beyond "a6xx.send_ac"
[PASS] PTRMAP present; PTU270 base preserved; no 269 ioctl
```

The patch was verified to apply *forward* to a reconstructed 271 baseline and
reproduce the built tree byte-for-byte.

## Running it

The device must be brought up **only after uptime > 300 s**
(`a6xx.gpu_init_delay_ms=300000`; writing `a6xx_gpu_load` earlier freezes it),
and the evidence must be pushed to the host, because `/userdata` rolls back on
the unclean reset and loses everything.

`/home/usb/.opencode/run271.sh` (device side) + `/home/usb/.opencode/collect.py`
(host side) already do both; rename the script to `run272.sh` and point it at
`272-halium-boot-a6xx-ptrmap.img`'s kernel. The host listener must be running
before the device script starts.

## Patch

`/home/usb/artifacts/gpu-bringup/patches/272-pointer-map.patch`
