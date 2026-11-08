# The BO upload path works. The kernel's BO-content dump is the thing that lies.

Corrects `CORRECTION-CACHE-COHERENCY.md` again — in the opposite direction, and
this time with a test whose logic was correct.

## The test

Two independent `glMapBufferRange()` calls on the same buffer are two views of
the *same* BO. So if a pattern written through the first is visible through the
second, the data really is in the BO.

```python
p1 = glMapBufferRange(GL_ARRAY_BUFFER, 0, SIZE, GL_MAP_WRITE_BIT)
ctypes.memset(p1, 0x5A, SIZE)          # write
glUnmapBuffer(GL_ARRAY_BUFFER)         # -> triggers fd_bo_upload()

p2 = glMapBufferRange(GL_ARRAY_BUFFER, 0, SIZE, GL_MAP_READ_BIT)   # 2nd view
got = ctypes.string_at(p2, 32)         # read through the second view
```

## Result

```
map1(WRITE)  same-ptr readback: 5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a  match=32
unmap1 -> 1 (TRUE)
map2(READ)   REMAP readback:   5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a  match0x5A=32/16
wrote 0xC3 via map3, unmap3 -> 1
map4         remap4: c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3  match0xC3=32/16
```

Two different patterns each round-tripped through two separate mappings.
**The upload path is sound.** `fd_bo_upload()` is not skipping anything.

(An earlier read of this same output claimed "DATA LOST" — that was a bug in my
analysis script: it counted matches over 32 bytes and compared against 16, so a
perfect result of 32 fell into the failure branch. The raw output above is
correct and the verdict printed with it was not.)

## What this means

The remaining anomaly is that the **kernel's own** view of the same BO shows
zeros:

- `msm_gem_get_vaddr()` → `vmap(pages, ..., pgprot_writecombine(PAGE_KERNEL))`
- and the `BOS[]` dump reads through it

Two independent userspace mappings agree with each other, and both disagree
with the kernel's vmap. So the kernel's `vmap()` is **not aliasing the same
physical pages** the userspace VMAs map. That is an aliasing bug in
`msm_gem_get_vaddr()` / `get_pages()`, not a cache coherency problem and not a
lost upload.

Note image 249 (map userspace buffers writecombine) therefore did **not** help
and was based on this bad measurement. It is harmless-but-pointless; the
parameter is `0644` and can be set to `N` at runtime.

## The real blocker

**We currently have no trustworthy way to observe GPU buffer contents from the
kernel.** Every "this buffer is empty" claim in this project — including the
shader ISA finding — came from a reader that does not alias the memory userspace
writes. Until that is fixed, we cannot see what the GPU is actually handed, so
we cannot diagnose the rendering failure from buffer contents at all.

## Next step

Make the kernel read a BO the same way userspace does, then re-run the
comparison. Concretely, in the `BOS[]` dump, replace `msm_gem_get_vaddr()` with
a mapping built from `get_user_pages()` on the object (the same pages the VMA
maps), or add a `sync_for_cpu()` + barrier before reading, and log the pointer
so the two views can be compared directly.

That single change makes the whole measurement chain trustworthy, and it is the
prerequisite for every remaining hypothesis about the rendering failure.

## Status

| claim | status |
|---|---|
| user buffer writes reach the BO | **PROVEN** (two-mapping round trip) |
| `fd_bo_upload()` skipping | **REFUTED** |
| cache coherency / writecombine as the cause | **UNSUPPORTED** — WC made no difference |
| image 249 (`bo_mmap_wc`) fixes rendering | **REFUTED** — no change |
| "shader ISA buffer is empty" | **STILL UNPROVEN** — measured with a non-aliasing reader |
| kernel BOS[] read aliases userspace | **NO** — demonstrated by the two-mapping test |
| GPU half-suspended, draws discarded | still valid (sysfs, independent of MMIO) |
| clocks/hwcg verified vs CAF | still valid (register readback) |
| zap loads/runs and is not the cause | still valid |