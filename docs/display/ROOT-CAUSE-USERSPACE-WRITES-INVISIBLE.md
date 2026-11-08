> ## RETRACTED — DO NOT TRUST
>
> This document is **wrong**. Its central claim rests on a null test: the buffers
> `pattern.py` wrote to never appeared in the submission's `bos[]` array, so the
> kernel had nothing to observe and "no pattern found" was guaranteed
> regardless of the true behaviour.
>
> See `CORRECTION-PATTERN-TEST-WAS-A-NULL-TEST.md`. The reasoning below about a
> mmap aliasing bug is unproven and is almost certainly a symptom of the flawed
> harness rather than a property of the driver.

# ROOT CAUSE ISOLATED: userspace writes to GEM buffers never reach the pages the GPU reads

The end of a long search. Everything below was re-confirmed on a validated
instrument, with the shader cache deleted and a cold compile forced.

## The measurement

Test (`pattern.py`): write `0xA5A5A5A5` via `glBufferData` and `0x5A5A5A5A` via
`glMapBufferRange`, force a real draw so both are submitted, then ask the kernel
what it is actually submitting.

```
bo[0]  1024 dwords  nonzero=1
bo[3]  1024 dwords  nonzero=72     <- command stream, intact
bo[4]  1024 dwords  nonzero=1
bo[5]  1024 dwords  nonzero=149    <- command stream, intact
bo[11] 1024 dwords  nonzero=244    <- command stream, intact
TOTALS: dwords=12126  A-pattern=0  B-pattern=0
```

**Neither user-written pattern appears anywhere.** The kernel sees only the
driver's own command streams.

## Why this is the root cause

Three independent facts pin it down:

1. **Two independent userspace mappings agree with each other** (`remap.py`:
   `0x5A5A5A5A` written through mapping 1 is visible through mapping 2).
2. **The kernel's two mappings agree with each other** (`BOSXCHK`: the
   `msm_gem_get_vaddr()` view matches a mapping built from `bo->sgt->sgl`, 8/8).
3. **Userspace and the kernel disagree** — the patterns are invisible to the
   kernel.

So userspace is writing into physical pages that are *not* the object's. The
GPU DMAs from the real pages, so it reads zeros.

This explains the whole project in one mechanism:

- **Shader ISA empty.** `upload_shader_variant()` runs, `fd_bo_upload()` runs,
  and `bo_map()` returned a valid pointer (a `memcpy` to NULL would segfault,
  and nothing crashes). The bytes went into the wrong memory.
- **Why nothing faults.** The GPU is handed correctly sized, correctly
  addressed, entirely zero buffers. Executing zeros is a no-op, not a fault —
  hence no interrupt, no GMU error, no hang-detect trigger across ~17 images.
- **Why every theory failed.** Clocks, zap, hwcg, coherency, the nomap flag and
  the disk shader cache were all innocent; they were being blamed for a bug
  upstream of all of them.

## Where the bug must be

In the GEM mmap path, i.e. the mapping userspace gets:

```c
int msm_gem_mmap(struct file *filp, struct vm_area_struct *vma)
{
	ret = drm_gem_mmap(filp, vma);          /* sets gem_vm_ops -> fault-based */
	if (ret) return ret;
	return msm_gem_mmap_obj(vma->vm_private_data, vma);
}
```

and the fault handler that actually supplies the pages:

```c
pgoff = (vmf->address - vma->vm_start) >> PAGE_SHIFT;  /* VMA-relative */
pfn   = page_to_pfn(pages[pgoff]);
vm_insert_mixed(vma, vmf->address, __pfn_to_pfn_t(pfn, PFN_DEV));
```

Note this uses `get_pages(obj)` (the `msm_obj->pages` array) while
`msm_gem_mmap_obj()` has a **cached branch that re-shunts the VMA onto the
object's shmem file**:

```c
fput(vma->vm_file); get_file(obj->filp);
vma->vm_pgoff = 0;
vma->vm_file  = obj->filp;
```

with a comment saying it is for `unmap_mapping_range()` to behave — "in
particular in the case of mmap'd dmabufs". The `MSM_BO_WC` and
`MSM_BO_UNCACHED` branches (flags `0x20000` / `0x20002` on all our buffers) do
**not** shim to shmem.

Whether `vm_file` is set decides which page source wins: the shmem file's own
fault handler, or `msm_gem_fault()` using `msm_obj->pages`. If those two page
sources ever diverge for an object, userspace and the kernel read different
memory — exactly what we measure.

That is the specific place to look next. It is also why `remap.py` passes: two
userspace mappings use the *same* (wrong) source, so they agree with each other
and still miss the real pages.

## What this supersedes

| earlier claim | status |
|---|---|
| shader ISA buffer is empty | **true, and now explained** |
| "small shader uploads, large one doesn't" | **artifact** — the single nonzero dword was a reloc/header word; it varies between runs (1, then 0) |
| disk shader cache replaying zeros | **refuted** — cache deleted, `MESA_SHADER_CACHE_DISABLE`+`MESA_GLSL_CACHE_DISABLE` set, cold compile, still empty |
| mmap truncated to one page | **refuted** — predicts head-nonzero/tail-zero; measured uniform zero across all 5 pages |
| `FD_BO_NOMAP` blocking the upload | **refuted** — `fd_bo_upload()` calls `bo_map()` directly, bypassing that guard |
| cache coherency / `bo_mmap_wc` | **refuted** — flipping it changed nothing |
| zap / secure mode | **refuted** — zap loads and runs, compute still dead |
| kernel mapping not aliasing GPU memory | **refuted** — BOSXCHK 8/8 |

## Corrections owed

The "large vs small shader" distinction I reported last round was noise from a
reloc-patched dword, and I should have histogrammed per page instead of counting
total nonzero dwords. A subagent review caught it; the corrected reading is
above.

## Not yet explained, and kept separate

`glClear` to opaque blue also reads back zero, and that involves no userspace
buffer write — the RB writes from the GPU side. It may be a second fault in the
resolve/readback path, or the same root cause via a different buffer. Do not
merge them until the mmap bug is fixed and re-measured.