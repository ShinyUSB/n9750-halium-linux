# RETRACTED — see `FINDING-SUBMISSION-IS-COMPLETE.md`

**The headline of this document is wrong. Do not act on it.**

The compute probe's `GEM_SUBMIT: 0 calls` does **not** mean Mesa never submits.
Reading `freedreno_ringbuffer_sp.c:313`:

```c
if ((in_fence_fd == -1) && !out_fence && !has_shared && should_defer(submit)) {
        dev->deferred_cmds += fd_ringbuffer_cmd_count(submit->primary);
        return 0; /* deferred, NOT submitted */
}
```

`should_defer()` returns true when `nr_bos <= 30` and `deferred_cmds <= 128`,
both true for a small compute dispatch. The submit was **queued for merging**,
not skipped. Two further problems with the original reasoning: `computeprobe.py`
is surfaceless (no drawable), and it aliased `glFinish` to `glFlush`.

Subsequent work proves the draw path submits and the submission is complete —
see `FINDING-SUBMISSION-IS-COMPLETE.md`.

---

# Original text (retained for history)

Supersedes the dangling-reference claim in `ROOT-CAUSE-DANGLING-IOVA.md`.

## Correction first

The "REAL DANGLING REF" at `bo[3]` dword 36 → `0x700026000` was **a false
positive**, and I caught it by dumping the raw words instead of trusting my own
filter:

```
[36] 0x00026000   [37] 0x00000007
[45] 0x00025000   [46] 0x00000007
```

`v = (sc[j+1] << 32) | sc[j]`, so those two dwords read as `0x00000007_00026000`.
The `0x00000007` high half repeats at dword 46 — they are register/offset
pairs, not addresses. My "page-aligned" filter was too weak, and I was one
build away from writing a kernel fix for noise. The image-260 scan still has
value, but it proved nothing on this run.

Also a correction to the adjacent claim: ten `glDrawArrays` do **not** produce a
byte-identical command stream. They add ~2 dwords each to `bo[3]` (72 → 88
nonzero), i.e. bare `CP_DRAW` packets with no accompanying state.

## The real finding: no submit is ever issued

`computeprobe.py` compiles and dispatches a compute shader. All GL calls return
`err=0x0000`, the magic constant is found in a `/dev/dri` mapping — and the
kernel BOS dump is **completely empty**. Not one `BOS[...]` line. The dispatch
never reached `a6xx_submit()`.

strace confirms it at the syscall level, with raw ioctl numbers so there is no
name-resolution ambiguity:

```
40x 0xc0186443  DRM_IOCTL_MSM_GEM_INFO
21x 0xc0106442  DRM_IOCTL_MSM_GEM_NEW
13x 0xc0186440  DRM_IOCTL_MSM_GEM_CPU_PREP
12x 0xc0406400  DRM_IOCTL_VERSION
 2x 0xc00c644a  DRM_IOCTL_MSM_GEM_MADVISE
 2x 0xc010640c  DRM_IOCTL_MSM_DEREGISTER_EVENT

DRM_IOCTL_MSM_GEM_SUBMIT (0xc0284046): 0 calls
```

**Mesa allocates buffers, uploads the shader into them, maps them — and never
submits.** Every GL call reports success. The work is simply never handed to the
GPU.

That reframes everything above it. The `bos[]` dumps we have been analysing came
from the **clear** path only. The shader BOs were never missing from a draw's
submission because **there was no draw submission to be missing from.**

This also explains, coherently, every symptom in the project:

- no pixels — nothing was submitted;
- no faults, `CP_INTERRUPT_STATUS` always 0 — there was nothing to fault on;
- fences signal — that is the clear's fence, and it does signal;
- the command processor retires cleanly — it retires the clear's stream;
- shader ISAs present in userspace memory but absent from every dump — correct,
  because they were never part of any submission.

## Why Mesa does not submit

Not yet determined. The suspect list, in order:

1. **The submit-queue handshake.** This tree is an image-167 hack: `Mesa 22.x
   creates a queue per context; it maps to our one legacy ring (id ignored)`.
   `msm_ioctl_submitqueue_new()` returns `id = 1` unconditionally and ignores
   priority. A mismatch in the queue-id contract could leave Mesa's flush path
   without a usable ring.
2. **Ringbuffer flush short-circuit** in `fd_ringbuffer_sp_flush` — the flush is
   gated on ringbuffer state that may not be reached when nothing else has
   forced a submit.
3. **Version-driven path selection.** `MSM_VERSION_MINOR` is 4. Mesa takes the
   `>= FD_VERSION_SOFTPIN` submit path. A different advertised version could
   select a different flush path.

All three are in the same file region and none is checkable without either the
Mesa source or another build — and every Mesa mirror reachable from this host
returns a 404 stub, so I could not read `ir3_gallium.c`/`msm_ringbuffer_sp.c` to
confirm which branch is taken.

## Next build, and it is small

The kernel can answer this directly. In `a6xx_submit()` nothing will appear
because it is never called, so instead instrument the **ioctl** path:

- log `msm_ioctl_submitqueue_new` / `_close` with `args->flags` and the returned
  id;
- log the first and last few bytes of `args->nr_bos` / `nr_cmds` at
  `msm_gem_submit` entry — currently we see nothing, which *proves* the ioctl
  never arrives, but a log makes it unambiguous and catches a partial/failed
  entry;
- most usefully: add a module parameter that makes `msm_ioctl_submitqueue_new`
  **echo back a unique id per call** and log the sequence, so we can see whether
  Mesa's queue id changes and whether submit then follows.

A cheaper userspace probe is worth running first, because it needs no build:
compare `glFinish()` against `eglSwapBuffers()` and against `glFlush()` in a loop,
checking after each whether the kernel BOS dump grew. If `eglSwapBuffers`
produces a submit where `glFinish` does not, the flush path is the bug and we
know exactly which entry point to attack.

## What this session closed

Kernel upload (32/32 on a 128-byte write) — proven sound. Userspace upload
(magic found in the VMA) — proven sound. Cache mode (`bo_force_cached` A/B) —
closed. Disk shader cache — closed. BO identity by size inference — retired;
it is what misdirected nineteen images. BO membership in submissions — **not
missing, because there are no draw submissions.** And every harness in the
project was drawing a zero-area triangle, now fixed in `vmascan3.py`.