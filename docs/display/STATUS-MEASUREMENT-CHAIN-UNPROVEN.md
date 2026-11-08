# Where we actually are: the measurement chain is still unproven

Status after image 250. Summary of the aliasing investigation and, I think, the
one clean test that remains.

## What image 250 established for certain

The kernel does tag submitted buffers:

```
BOSALIAS[seq=1] bo[6] wrote 0x5c5c5c5c x16 at ffffff8020536000 (ndw=8192)
BOSALIAS[seq=1] bo[7] wrote 0x5c5c5c5c x16 at ffffff801b07d000 (ndw=1024)
... 6 buffers tagged
```

## What it did not establish

**Whether `msm_gem_get_vaddr()` aliases the mapping userspace writes.** Three
attempts, each confounded:

| attempt | confound | result |
|---|---|---|
| userspace remap after draw | compared against `0x5A` instead of `0x5A5A5A5A` (I memset *bytes*) | "our pattern 0/16" was my bug; tag genuinely absent |
| same, with `bo_mmap_wc=N` | tests the original cached path | tag still absent — so **not** the image-249 change |
| persistent mapping | `GL_MAP_PERSISTENT` map **failed**; nothing was written at all | run is void |

So the honest position: the evidence *leans* towards the kernel's vmap being
separate memory, but no test has yet been clean, and I am not going to call it.

Worth noting the result is the same with `bo_mmap_wc` both `Y` and `N`, which at
least rules out my image-249 change as the cause.

## Why userspace is the wrong place to test this

Every userspace check has at least three ways to mislead:

1. freedreno **invalidates and re-uploads** buffers after use, which can restore
   Mesa's own copy over anything the kernel wrote;
2. a `PIPE_MAP_READ` may be served by a **GPU blit to staging**
   (`resource_transfer_map_staging`) rather than a CPU read of the BO, so it
   does not test the CPU mapping at all;
3. `GL_MAP_PERSISTENT` can simply fail, leaving the buffer unwritten.

## The one clean test: compare physical addresses in the kernel

No userspace, no confounds. At submit time, for each BO, log:

- `virt_to_phys()` of the pointer `msm_gem_get_vaddr()` returns, and
- the physical address of the first page in the SGT that the GPU is actually
  given (`msm_gem_get_sgt()` / `sg_phys(sg->sgl, 0)`)

If those differ, the kernel's vmap is provably not the memory the GPU is handed,
and every kernel-side content read this project has done is invalid. If they
match, the mapping aliases and the zeros must be a region/offset problem —
noting that image 221's comment records exactly that mistake once already (a
1024-dword cap that landed on a UBWC metadata plane boundary, so "the render
target is all zeros" was reading metadata).

This is a few lines in the same dump site and one image.

## Also still true

- The BOS[] dump cap is now 8192 dwords, but render targets may be larger; the
  dump only ever prints the first `min(bo_size, 8192)` dwords, so a UBWC colour
  plane starting at 0x1000 is covered but larger allocations are not.
- Image 249's `bo_mmap_wc` changed nothing measurable. It is `0644` and can be
  set to `N`; it should probably be reverted to default `N` once we stop
  investigating, since it buys nothing and costs performance.

## Honest bottom line

We still do not have a trustworthy way to read GPU buffer contents from the
kernel, and therefore still do not have a trustworthy way to see what the GPU is
being handed. Every confident claim made along the way about "empty buffers",
"lost uploads" and "cache incoherency" has been withdrawn. The one thing that
survives every falsification is the original, boring observation: the CP domain
works perfectly and the shader domain produces nothing.

I would rather spend the next image on the physical-address comparison than on
another round of rendering hypotheses, because until that is settled we are
guessing about our own instruments.