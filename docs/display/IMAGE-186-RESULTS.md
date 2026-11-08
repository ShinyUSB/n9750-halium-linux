# Image 186 — results. The GPU was never the problem. Nothing is being submitted.

Kernel `#128`. This is the most informative image in the series, and it
redirects the whole investigation.

## What the auto-probe produced

Two dumps, both captured automatically by the kernel with no ssh involvement,
exactly as designed:

```
[240.15] CPDUMP[auto-rest] sw : rb_iova=0x1000 rb_size=32768 wptr_dw=99 next_dw=99
[240.15] CPDUMP[auto-rest] hw : RB_BASE=0x1000 RB_CNTL=0x20c RB_RPTR_ADDR=0x0
[240.15] CPDUMP[auto-rest] hw : RB_RPTR=0x63 RB_WPTR=0x63 SQE_CNTL=0x1 SQE_BASE=0x9000 (sw 0x9000)
[240.15] CPDUMP[auto-rest] irq: INT_0_STATUS=0x0 INT_0_MASK=0x3d0e242 CP_INTERRUPT_STATUS=0x0
[240.15] CPDUMP[auto-rest] st : RBBM_STATUS=0x1 linux_irq=487 irqs=0
[240.21] CPDUMP[auto-rest] CPF: t0=0x... t1=0x... delta=39817854 => CP is CLOCKING
[252.21] CPDUMP[auto-post] (identical values)
[252.27] CPDUMP[auto-post] CPF: delta=35914480 => CP is CLOCKING
```

And a third manual dump at t=331.7, taken while `egl6.py` was blocked in
`glFinish`, and a fourth at t=400 after a real draw. All four identical.

### The mask decodes exactly right

`INT_0_MASK = 0x03D0E242`. Reconstructing `A6XX_INT_MASK` from the bit
definitions in `a6xx.xml.h` gives `0x03D0E242` — a bit-exact match, with
`CP_CACHE_FLUSH_TS` (bit 20, `0x00100000`) present. So the interrupt is **not**
masked off, and no "unmask the completion interrupt" fix is available. The
consultants' remaining idea (a threaded IRQ) is irrelevant: the counter is
zero because nothing is being raised, not because the handler is wrong.

### The decisive number: `wptr_dw=99`

`rb->cur - rb->start` is **99** in every single dump, and the hardware agrees:
`CP_RB_RPTR == CP_RB_WPTR == 0x63` (=99). That is precisely where the bring-up
selftest left the ring. Not one dword has been appended by anything since.

`a6xx_submit()` is the only writer of those registers (`OUT_RING()`), it is
intact, and it is wired into `a6xx_gpu_funcs.submit`. So **`a6xx_submit()` is
never called.**

## Confirmed by strace, independently

Tracing every ioctl Mesa issues:

```
76  DRM_IOCTL_AMDGPU_BO_LIST   (really MSM_GEM_INFO)
39  DRM_IOCTL_AMDGPU_CTX       (really MSM_GEM_NEW)
12  DRM_IOCTL_VERSION
 9  DRM_IOCTL_MSM_GET_PARAM
 2  DRM_IOCTL_GET_CAP
 1  FIOCLEX
```

**`DRM_IOCTL_MSM_GEM_SUBMIT`: zero occurrences, out of 184 ioctls.**

Mesa opens a real FD640 context, compiles shaders, links a program, builds a
complete FBO, issues `glDrawArrays(GL_TRIANGLES,0,3)` — and never submits a
single command. The ring never moves. The driver is never asked to do anything.

## Ruled out along the way

- **Not a GPU/hardware fault.** The CP perf counter advances ~35-40 million per
  50 ms sample in every dump. The CP is alive and clocking. A dead or wedged
  CP cannot produce that.
- **Not the interrupt mask.** Decoded bit-exact, above.
- **Not GEM allocation.** `DRM_IOCTL_VERSION` returns 0 and `glBufferData` on a
  fresh buffer succeeds (`err=0x0000`) — that is a genuine `MSM_GEM_NEW` plus
  driver-side allocation, and it works.
- **Not the empty-command-stream red herring.** I wrote `egl8.py` specifically to
  test this: a linked GLSL ES 3.00 program plus `glDrawArrays` guarantees a
  non-empty stream that freedreno cannot legitimately skip. Shaders compile,
  program links, `glDrawArrays` returns 0 — and `wptr_dw` is *still* 99.
- **Not the GBM platform, not the context, not the FBO.** All verified working
  in every run.

## Where this leaves the investigation

Every previous image was asking "why does the GPU core IRQ not fire?" The
answer is that **there is no GPU work to complete**. The IRQ was never going to
fire because nothing was ever submitted. Chasing the interrupt path — the
threaded-IRQ idea, the master-enable idea, the ring-handoff idea — was all
downstream of a false premise.

The question is now sharply two-sided, and both sides are cheap to test:

**(a) Userspace.** Something makes freedreno decide it has nothing to submit.
Worth knowing: freedreno gates submission on `TU_DEBUG`/`banned` state and on
whether the GPU is considered alive. A device it believes is not alive will
silently drop work. Our kernel's adreno ID or a capability bit could be
mismatched enough that freedreno takes a degraded path.

**(b) Kernel.** The submit ioctl could be failing early — before it reaches
`a6xx_submit` — with freedreno swallowing the error. But strace shows the ioctl
is *never issued*, which points at (a) rather than (b). Confirming that the
ioctl path is even reachable, by hand-issuing `MSM_IOCTL_GEM_SUBMIT`, would
settle it.

The cheapest next step is (b)-style verification without a new kernel: hand
-craft a submit ioctl against a real GEM object and see whether the driver
accepts it. If a hand-made submit works, the bug is entirely in freedreno's
decision to submit. If it fails, we get the errno and land in the driver.

Recorded while the phone is still alive and the GPU still loaded — worth not
wasting that state.