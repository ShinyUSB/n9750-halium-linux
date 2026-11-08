# memptrs_iova == 0, and the ring may be colliding with the GMU firmware window

Date: 2026-11-11. Found by reading `RINGHW-W` lines that were already in the
image-208 boot log. **Not yet fixed — this is analysis plus a proposed next
measurement.**

## The confirmed fact

From `/root/t208.log`, during `adreno_hw_init`:

```
RINGHW-W: CP_RB_CNTL      before=0x0                    after=0x20c
RINGHW-W: CP_RB_BASE      before=0x0:00000000 val=0x1000 after=0x0:00001000
RINGHW-W: CP_RB_RPTR_ADDR before=0x0:00000000 val=0x0    after=0x0:00000000
```

`CP_RB_BASE` was programmed with `gpu->rb_iova = 0x1000` and the write took.
`CP_RB_RPTR_ADDR` was programmed with `adreno_rbmemptr(adreno_gpu, rptr)` and
**that value is 0**, so the write did nothing.

## Why that value is 0

```c
/* adreno/adreno_gpu.h:107 */
#define adreno_rbmemptr(adreno_gpu, member) \
	((adreno_gpu)->memptrs_iova + offsetof(struct adreno_rbmemptrs, member))
```

```c
/* msm_ringbuffer.h:29 */
struct msm_rbmemptrs {
	volatile uint32_t rptr;    /* offset 0 */
	volatile uint32_t fence;   /* offset 4 */
};
```

So `rptr` is at offset 0 and `CP_RB_RPTR_ADDR == memptrs_iova` exactly.
`CP_RB_RPTR_ADDR` reads 0 ⇒ **`adreno_gpu->memptrs_iova == 0`.**

Chain:

```
adreno_gpu.c:594   msm_gem_kernel_new(..., &memptrs_bo, &memptrs_iova)
  -> msm_gem.c:544  msm_gem_map_vma(...)
    -> msm_gem_vma.c:255 iommu_aspace_map_vma()
      -> msm_gem_vma.c:345  vma->iova = vma->node.start << PAGE_SHIFT;
```

`vma->iova` is 0 ⟺ `drm_mm_insert_node()` handed out `node.start == 0`, i.e.
`memptrs` was the **first** allocation in the address space and the aspace's
`drm_mm` starts at `aperture_start >> PAGE_SHIFT == 0`
(`msm_gem_vma.c:275`).

The allocation **reported success** — there is no "could not allocate memptrs"
in dmesg — so nothing errored. This is a silent-zero, not a failure.

## Why that breaks the fence (the confirmed symptom)

`a6xx_submit()` ends with the cache-flush-timestamp event:

```c
OUT_PKT7(ring, CP_EVENT_WRITE, 4);
OUT_RING(ring, CACHE_FLUSH_TS | (1 << 31));
OUT_RING(ring, lower_32_bits(rbmemptr(ring, fence)));   /* memptrs_iova + 4 */
OUT_RING(ring, upper_32_bits(rbmemptr(ring, fence)));
OUT_RING(ring, submit->seqno);
```

With `memptrs_iova == 0` the GPU is told to write the fence to **IOVA 0x4**.

That address is inside the GMU firmware window established by image 202:

```
ALLOC202 iova=0x0      size=0x4000    <- icache   (CM3 instruction cache)
ALLOC202 iova=0x4000   size=0x3c000   <- dcache
ALLOC202 iova=0x44000  size=0x3c000
ALLOC202 iova=0x60000000 size=0x1000  <- dummy
ALLOC202 iova=0x60006000 size=0x1000  <- log
```

So the fence write lands in the CM3's instruction cache window, not in the
fence memptr, and `memptrs->fence` is never updated. That is precisely the
observed `fence: 0/1` with `submitted=1`, and it is why the hangcheck
correctly declares a lockup on every tick.

It also means `CP_RB_RPTR_ADDR = 0`, so the GPU has no valid mirror address for
its read pointer.

## The bigger worry: the ring may be in the same collision

`gpu->rb_iova = 0x1000`. That is **also inside the GMU icache window**
(`iova=0x0 size=0x4000`).

If the CP's ring reads and the CM3's icache share an SMMU context bank, then
`RB_BASE = 0x1000` points the GPU at the *firmware's* pages rather than the
ring's, and the GPU would faithfully fetch 15 dwords of firmware and execute
nothing. That matches the 205 observation exactly:

```
before submit   WPTR 0x63  RPTR 0x63
after  submit   WPTR 0x72  RPTR 0x72     <- RPTR chases WPTR: it IS fetching
CP_SCRATCH_REG2: 0                       <- but executes nothing
```

RPTR chasing WPTR proves the GPU is walking the ring. If the ring it is walking
is not the ring the CPU wrote, every other observation falls into place.

**Uncertainty, stated plainly:** I have not confirmed whether the GMU icache
mapping and `gpu->aspace` mappings share an SMMU context bank. If they are
separate banks there is no collision and this whole line of reasoning is wrong.
The `memptrs_iova == 0` fact stands either way and still breaks the fence.

## Ordering note

`a6xx_gpu_init()` → `adreno_gpu_init()` allocates memptrs, and that happens
**before** `adreno_load_fw()` in `adreno_load_gpu()`. So `memptrs` claims iova 0
first and the GMU block map lands on top of it. Neither reports an error.

## Proposed next step (measurement, not a guess)

One image that only adds printk, changing no behaviour:

1. print `adreno_gpu->memptrs_iova`, `gpu->rb_iova`, and the SMMU context bank
   / domain id used for each mapping
2. print the first 16 dwords of the ring from the **CPU** side
   (`ring->start[0..15]`) immediately after `a6xx_submit()` returns, so we can
   confirm the command stream is really in the buffer the CPU wrote
3. print `memptrs->fence` / `memptrs->rptr` at the same moment

Outcomes:

| observation | conclusion |
|---|---|
| ring CPU contents hold the expected packets | CPU side is fine; the GPU is reading the wrong memory — collision or missing doorbell |
| `memptrs_iova == 0` and GMU blocks also start at 0 | confirmed address-space collision; fix is to stop handing out iova 0 / reserve the GMU window |
| `CP_RB_RPTR_ADDR` nonzero after a fix, fence still stuck | fence path fixed, execution problem is elsewhere |

## Why this was missed for 200 images

`memptrs_iova` is **never printed anywhere in the driver** — `grep -rn
memptrs_iova` returns four hits, all of them the allocation and cleanup, none a
diagnostic. The value was silently 0 the entire time. The `RINGHW-W` diagnostic
added back in image 129 *did* print it, as `val=0x0`, but it was read as an
unremarkable zero rather than as the address of the fence.

Lesson: a diagnostic that prints an address should be read as an address.

## Related open item

`CP_SCRATCH_REG2 == 0` is still unexplained and may be a false signal — whether
CP_SCRATCH_REG is even readable from the CPU on A6XX is being checked
separately. If it is not readable, the only real evidence of non-execution is
the fence, and the address-space explanation above becomes the primary
hypothesis rather than one of several.
