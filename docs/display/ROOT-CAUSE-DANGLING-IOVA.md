# RETRACTED — the DANGLING scan is a false-positive generator

See `FINDING-SUBMISSION-IS-COMPLETE.md`. The image-260 scan tests **exact
base-IOVA equality**, so any relocation pointing into the *middle* of a
submitted buffer is reported as dangling. On a verified-complete capture
(11/11 BO headers captured), all 13 relocation targets in the command stream
resolve inside submitted BOs — `inside=13 outside=0` — and every one of the
scan's 7 hits was a valid offset into a submitted buffer.

The filter must test *containment* (`iova <= target < iova + size`), not
equality. This is the second time this premise produced a false result, and
both times the cause was this same weak filter. Do not cite `DANGLING` lines
as evidence.

---

# Original text (retained for history)

# ROOT CAUSE MECHANISM: the command stream references a buffer the submission never carries

Image 260 (`dangling`) + harness `vmascan3.py`.

## The measurement

Image 260 gathers each submission's full IOVA set, then scans every dumped
buffer for 64-bit values in the SMMU aperture that no submitted BO provides.
One genuine hit:

```
seq=2 bo[3] dword 36 -> iova=0x700026000   page-aligned   REAL DANGLING REF
seq=2 bo[3] dword 74 -> iova=0x700000007   not aligned   false positive (PM4 reg-write header)
seq=1 bo[11] dword 280 -> 0x700021010      not aligned   false positive

submitted: 0x700016000 0x700022000 0x700025000 0x70002d000 0x700038000 0x700039000
```

`bo[3]` is a command stream. At dword 36 it carries **0x700026000**, and **no
submitted buffer provides that address**.

The other two candidates were rejected on a principled filter rather than
eyeballed: a real BO IOVA is page-aligned, while `0x70000007` is a PM4
register-write header that merely happens to look aperture-shaped, and
`0x700021010` is unaligned. Exactly one real dangling reference.

## What this means

Mesa **does** write the shader address into the command stream, and Mesa
**does not** include the shader buffer in the submission. Those two facts
together are the whole bug:

1. `OUT_RELOC(ring, so->bo, ...)` runs — it writes `so->bo->iova` into the
   stream (dword 36 holds it).
2. `fd_submit_append_bo()` does **not** run for that BO, so it never reaches
   `req.nr_bos` and never reaches the kernel's `bos[]`.
3. The GPU is pointed at 0x700026000, which this submission does not map.

That is a complete explanation for every symptom: the CP executes and retires
the stream cleanly, no buffer fault is raised, fences signal, and the shader
domain executes nothing at all because the program it fetched is not there.

It also explains why the ISAs are "empty" in every kernel-side reading — they
were never part of the submission, so of course no dump ever showed them. And
why the userspace VMA still holds correct ISA: the bytes exist, they are simply
not handed to the GPU.

## The code path, and where it diverges

`bos[]` has exactly one entry point:

```c
/* msm_ringbuffer_sp.c:134-141 */
for (i = 0; i < fd_submit->nr_bos; i++) {
        submit_bos[i].flags    = fd_submit->bos[i]->reloc_flags;
        submit_bos[i].handle   = fd_submit->bos[i]->handle;
        submit_bos[i].presumed = 0;
}
req.nr_bos = fd_submit->nr_bos;
```

and `fd_submit_append_bo()` is reached from two places — the immediate path and
the deferred state-object path:

```c
/* freedreno_ringbuffer_sp_reloc.h:94-108 */
if (ring->flags & _FD_RINGBUFFER_OBJECT) {
        /* deferred: remember it on this ringbuffer object */
        for (i = 0; i < fd_target->u.nr_reloc_bos; i++)
                if (!fd_ringbuffer_references_bo(ring, target_bo))
                        APPEND(&fd_ring->u, reloc_bos, fd_bo_ref(target_bo));
} else {
        /* immediate */
        for (i = 0; i < fd_target->u.nr_reloc_bos; i++)
                fd_submit_append_bo(fd_submit, fd_target->u.reloc_bos[i]);
}
```

So when a **state-object ringbuffer** references a target, the target BO is
recorded on that object rather than appended to the submit, and something must
later drain it. On this configuration that drain is not happening for the
shader BO.

Checked and excluded: `bo_limit` in `msm_ringbuffer_sp.c:123` is
`4096 / sizeof(struct drm_msm_gem_submit_bo)` = 256, and it only chooses between
a stack array and `malloc` for the copy — it is not a cap. No
`MAX_SUBMIT_BOS` or similar limit exists in the tree.

## Where this leaves the diagnosis

The mechanism is now known; the remaining question is which Mesa bookkeeping
step fails to merge the shader BO, and that is **not fixable from the kernel** —
we cannot rebuild Mesa here (no aarch64 cross-toolchain, Debian package on
device).

Two ways forward, both cheap:

1. **Decide whether it is ours at all.** Our `drm_msm_gem_submit` is 40 bytes
   and `drm_msm_gem_submit_bo` is 16, matching Mesa exactly, and submits
   succeed — so this is not a UAPI struct mismatch. But the *legacy* CAF submit
   path Mesa is using may not be the interface Mesa expects for buffer
   bookkeeping. Worth one experiment: raise `FD_VERSION_SOFTPIN` semantics by
   advertising a higher `MSM_VERSION_MINOR` and see whether freedreno switches to
   the modern submit path, where BOs are gathered differently.
2. **Kernel-side workaround.** If the shader BO is genuinely pinned (it must
   have an IOVA — 0x700026000 was allocated) but omitted from the array, the
   driver could, at `validate_objects()` time, scan the command stream for
   aperture addresses and ensure any referenced address is backed by a submitted
   buffer — or at minimum log it. That turns a silent dangling pointer into a
   diagnosable one and is a small, safe change.

## Also recorded this round

- **Every harness in this project drew a zero-area triangle** — all three
  vertices at the same position, in `bigshader.py`, `clearonly.py`, `alias3.py`,
  `vmascan2.py`. Fixed in `vmascan3.py` with real positions derived from
  `gl_VertexID`. It was **not** the cause (the ISAs are still unsubmitted with a
  genuine triangle), but it was a confound sitting under every prior result.
- `FD_RD_DUMP=full` (freedreno's RenderDoc dumper, which would show every
  buffer from Mesa's own view) produces nothing on this device — Mesa 22.3.6
  predates it.
- Everything else stays closed: kernel upload (32/32 on a 128-byte write),
  userspace upload (magic found in the VMA), cache mode (`bo_force_cached` A/B),
  disk shader cache, mmap offsets (image 259's `mmoff` join), and
  `glGetBufferSubData` as evidence.