# CORRECTION: the "empty shader ISA" was a measurement artifact. The real fault is cache coherency.

This retracts the central claim of `ROOT-CAUSE-SHADER-ISA-EMPTY.md` and replaces
it with something better supported.

## The experiment that settles it

Map a GL buffer, write a known pattern, then read it back **twice**:

1. from the **same pointer** `glMapBufferRange()` returned
2. via `glGetBufferSubData()` (a different path)

Result:

```
bufB same-pointer readback:  first16bytes=5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a  matching=32/16
userspace readback (POST-draw) bufB: 0x00000000 0x00000000 ...  matching=0/8
kernel BOS[] dump:                       B-pattern=0
```

**The bytes are in userspace memory.** Reading the same pointer returns them.
Every other reader sees zeros.

## Why

`msm_gem_mmap_obj()` (`msm_gem.c:239`) gives userspace a **cacheable** mapping
by default:

```c
} else {
	/* cached objs shunted to their own shmem file ... */
	vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
}
```

Only `MSM_BO_WC` and `MSM_BO_UNCACHED` objects get non-cacheable mappings.

The kernel's own read path, used by the `BOS[]` instrumentation, is
**writecombine**:

```c
msm_obj->vaddr = vmap(pages, obj->size >> PAGE_SHIFT,
		VM_MAP, pgprot_writecombine(PAGE_KERNEL));
```

`pgprot_writecombine` yields a **non-shareable** mapping, which is not coherent
with the shareable cacheable mapping userspace writes through. So the kernel's
dump was reading stale memory and reporting "all zeros" for a buffer that is in
fact fully populated.

**Every "the buffer is empty" measurement in this project is therefore
unreliable.** That includes the bo[1] "0 nonzero of 5120 dwords" result, which
was the basis for `ROOT-CAUSE-SHADER-ISA-EMPTY.md`.

## What this actually explains

The same incoherency explains the rendering failure directly:

> A user writes the shader ISA (or any buffer) through a cacheable mapping. The
> data sits as **dirty cache lines** and is never written back to DRAM. The GPU
> DMAs from DRAM and therefore executes **zeros**.

That is precisely the observed signature: CP domain perfect, shader domain
silent, no fault, no interrupt, no GMU error, no hang-detect trigger. Executing
a zero ISA is a no-op, not an error. And it explains why *every* write path
failed identically — solid-colour clears through the RB, compute SSBO writes,
and shader ISA loads all read DRAM that never received the data.

It also explains the anomaly that an empty ISA could never account for: even
`glClear` produces nothing, because the render target's contents never became
visible either.

## Where the fix belongs

Candidates, roughly in order of directness:

1. **Map GPU BOs non-cacheable.** The tree already has `MSM_BO_WC` /
   `MSM_BO_UNCACHED` handling in `msm_gem_mmap_obj()`. If Mesa/DRM requests
   those flags for these objects, userspace writes go straight to memory and the
   problem disappears. Check whether the flags are actually reaching the object,
   and whether freedreno sets them.
2. **Fix the coherency attributes** so the shareable cacheable mapping *is*
   coherent with the GPU's DMA (inner-shareable + coherent SMMU). This is the
   architectural fix, and it is what makes the default cacheable path correct.
3. **Explicit cache maintenance** (clean/writeback the BO range) before the
   GPU is kicked off. Correct but a band-aid; it costs performance.

Note the measurement problem must be fixed alongside: the `BOS[]` dump should
read through a **coherent** path (e.g. `dma_buf_begin_cpu_access()` or a
cacheable temporary mapping), otherwise it will keep lying.

## Status of earlier conclusions

| claim | status |
|---|---|
| GPU half-suspended, draws discarded | **still valid** (sysfs/pm evidence, independent of MMIO) |
| Clocks/hwcg verified vs CAF | **still valid** (regs read back correctly) |
| zap loads and runs, and is not the cause | **still valid** (compute still zero with zap) |
| `pm_hold` works (`suspended +=0`) | **still valid** (sysfs accounting) |
| "shader ISA buffer is empty" | **RETRACTED** — non-coherent read artifact |
| "userspace buffer data never reaches the kernel" | **RETRACTED** — data is present; readers are incoherent |

## Methodological note

Two measurement traps were hit this session, both worth guarding against:

1. **Non-coherent reads.** Reading GPU memory through a writecombine mapping
   while userspace writes through a cacheable one reports stale data. Always
   corroborate with a read through the *same* mapping that wrote.
2. **Software fallback.** Mesa silently falls back when the GPU pipe cannot be
   allocated, and a software clear reads back perfectly. All harnesses now
   print the renderer and abort unless it is `FD640`.