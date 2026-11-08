# The upload path fails silently — userspace buffer data never reaches the kernel

Continuation of `ROOT-CAUSE-SHADER-ISA-EMPTY.md`. The discriminator run is done
and it narrows the fault sharply.

## The test

Wrote two known patterns into GPU buffers from userspace via both upload routes,
then asked the **kernel** (not userspace) what it was actually submitting:

- **A** `0xA5A5A5A5` via `glBufferData` — Mesa's ordinary transfer/upload route
- **B** `0x5A5A5A5A` via `glMapBufferRange(GL_MAP_WRITE_BIT)` — the direct CPU
  mapping route, the same one the `FD_BO_NOMAP` shader BO uses

Run on a genuine `FD640` (zap loaded, `PMHOLD248` held, `runtime_status=active`),
with the `ibdump` instrumentation capturing every submitted buffer's contents.

## Result

```
TOTALS: dwords=12319  nonzero=467  A-pattern=0  B-pattern=0
```

**Neither pattern appears anywhere in what the kernel is submitting.**

| BO | iova | dwords | nonzero | A | B |
|---|---|---|---|---|---|
| bo[1] (shader ISA) | 0x700027000 | 5120 | **0** | 0 | 0 |
| bo[2] | 0x70003a000 | 1024 | **0** | 0 | 0 |
| bo[3] | 0x70002d000 | 1024 | 72 | 0 | 0 |
| bo[5] | 0x70002e000 | 1024 | 149 | 0 | 0 |
| bo[11] | 0x70002c000 | 1024 | 244 | 0 | 0 |

The **command-stream BOs are perfectly intact** (bo[3], bo[5], bo[11] carry real
command data). So this is not a general "nothing reaches the kernel" failure —
it is specific to **buffer data uploaded through the transfer/map path**.

And the kernel logs **nothing**: no `msm_gem` error, no transfer error, no
warning. The failure is completely silent.

## Why that points at `fd_bo_map()`

In Mesa (`freedreno_resource.c`):

```c
void fd_bo_upload(struct fd_bo *bo, const void *src, unsigned offset, unsigned size)
{
        char *dst = fd_bo_map(bo);
        if (dst) {
                memcpy(dst + offset, src, size);
                fd_bo_unmap(bo);
        }
        /* no else, no error, no log */
}
```

If `fd_bo_map()` returns NULL, the copy is **silently skipped**: no syscall
error reaches the kernel, nothing is logged anywhere, and the destination buffer
stays zero. That is precisely the observed signature.

Command buffers do not go through this path — they are written through a
persistent CPU mapping from the ring allocator — which is exactly why they
survive while uploaded buffers do not.

This also explains the empty shader ISA: `ir3_gallium.c:116` does
`fd_bo_upload(v->bo, v->bin, 0, v->info.size)`. If the map fails, the SP gets a
correctly-sized, correctly-addressed, **entirely zero** buffer, and executes
nothing — silently, with no fault.

## Honest caveats

- The userspace `glGetBufferSubData` readbacks also returned zeros, but those
  were taken **before** the draw in the first run; an unreferenced buffer may
  legitimately read back as zero. A post-draw readback was added to remove that
  confound but the run was lost to a device freeze before completing. So
  "userspace sees nothing" is **not yet established**; the kernel-side absence
  is the solid result.
- The mechanism above is a strong fit, not a proof. `fd_bo_map` can also return
  a mapping that does not alias the GPU-visible pages, which would produce the
  same zeros. Both are kernel-mmap questions.

## Next step (no flash needed to start)

Determine whether a GPU BO's CPU mapping aliases the memory the kernel/GPU sees.
Either:

1. From userspace: `glMapBufferRange` a buffer, write a pattern, and have the
   kernel dump that BO — the test just run, but with the readback moved after
   the draw. If the pattern is still absent, mmap is non-aliasing.
2. In the kernel: log inside `msm_gem_object_map()` / the BO mmap handler —
   return value, and whether the object is marked CPU-mapped at submit time.
   A `pr_info` of the mapped pointer vs `bo->vaddr` at submit time would
   settle aliasing immediately.

Option 2 is the more direct one, and one small image would answer it.

## Methodological note

An earlier run produced a **false positive** worth recording: `PIXEL=[0,0,255,255]`
looked like the clear finally working, but the GPU had never been initialised
that boot (`runtime_status=unsupported`), Mesa had fallen back to **software
rendering** (`fd_pipe_new2: allocation failed`), and software clears read back
perfectly while the GPU does nothing. All harnesses now print the renderer and
abort unless it is `FD640`. Any result from a run where the GPU was not
confirmed up is invalid.

Also: the init helper was not idempotent and re-triggered GPU attach on an
already-initialised device, which wedged it. The sequence must check
`runtime_status` first.