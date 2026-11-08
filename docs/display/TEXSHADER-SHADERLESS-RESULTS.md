# Texture-shader + shaderless tests: both hypotheses dead, and a new fact

Run on `#130` with the GPU already loaded. No kernel change, no lottery ticket.
Harness: `harness/texshader.py`.

## TEST A — does a texture-sampling fragment shader compile?

The premise was that `MAX_TEXTURE_IMAGE_UNITS = 0` (vs GLES 3.1's required 16)
proved the screen object was degenerate. So: compile a fragment shader that
samples a texture. If the limit were real and enforced, it must fail.

```
MAX_TEXTURE_IMAGE_UNITS = 0
vertex    compiled=1 log_len=0
fragment  compiled=1 log_len=0
--> TEST A RESULT: fragment sampling compiled = 1
              (COMPILES, so MAX_TEXTURE_IMAGE_UNITS=0 is NOT enforced)
program linked=1
```

**The shader compiles.** So `MAX_TEXTURE_IMAGE_UNITS=0` is a reporting
artefact — the limit is declared but not enforced by the shader compiler.

This kills my own hypothesis cleanly. I had been treating those two GLES 3.1
minimum violations as "the first hard internal inconsistency in userspace" and
suggested they pointed at partial screen initialisation. They don't. They are
most likely another miscounted `PIPE_CAP` constant in `freedreno_screen.c`,
the same class of thing as the `16384` values Muse Spark told me were
hardcoded. **Two independent "smoking guns" have now turned out to be
hardcoded PIPE_CAP constants.** Worth internalising: in this driver, GL limits
are not evidence.

## TEST B — does a shaderless draw submit?

If the drop were in program emit, a draw using the default program should get
through.

```
B1 clear-only        err=0x0000   flush err=0x0000   -> wptr_dw=99
B2 shaderless draw   err=0x0000   flush err=0x0000   -> wptr_dw=99
B3 shaderless 100k   err=0x0000   flush err=0x0000   -> wptr_dw=99
```

Not even a bare `glClear(GL_COLOR_BUFFER_BIT)` with an explicit flush moves the
ring. So the drop is **not** in program emit, not in shader handling, and not
in primitive culling. All six probes across every harness ever written agree:

```
CPDUMP[aliveness]      -> 99     CPDUMP[tex]          -> 99
CPDUMP[b1clear]        -> 99     CPDUMP[huge]         -> 99
CPDUMP[b2shaderless]   -> 99     CPDUMP[v0]           -> 99
CPDUMP[b3big]          -> 99
```

**`wptr_dw` has never been anything but 99.** Not once, for any input, across
five harnesses and three kernel images.

## What this eliminates, and what it sharpens

Dead now, with evidence rather than argument:

| hypothesis | killed by |
|---|---|
| batching / empty-batch early-out | explicit `glFlush` on a 1M-vertex draw |
| early draw discard (cull/scissor/mask) | same |
| program emit drops the draw | B1: a bare `glClear` also does nothing |
| shader/compile related | B1: no shader involved |
| screen object degenerate (limits) | TEST A: the limit is not enforced |
| `DRIVER_SYNCOBJ` | enabled, rc=0, no behaviour change |
| `MAX_FREQ` garbage | only used for perf/debug |
| GPU/hardware | CP clock alive, `FAULTS=0`, no SMMU/power faults |

Sharpened by elimination: **freedreno never submits *anything*, at all, ever —
not even a plain clear.** That is a stronger and stranger statement than
"draws are dropped". A driver that cannot submit a clear is not
batch-skipping; something structural is preventing the ioctl from ever being
reached.

That points hard at the remaining structural candidates, and specifically
suggests the fault is *above* the batch, in whatever decides to call
`pipe->flush()` at all — or in the submitqueue. Note again that
`DRM_MSM_SUBMITQUEUE_NEW` was never issued in the full-process trace despite
`PRIORITIES=3`. If freedreno 22.3.6 expected a submitqueue and never got one,
a NULL-queue guard in `msm_submitqueue_flush()` would produce exactly this:
every flush a silent no-op, zero submits, zero errors, and a teardown crash.
It would also be consistent with the SIGSEGV at teardown, which a queue
use-after-free would explain.

**I am not claiming that yet.** It is the best remaining hypothesis and it
happens to explain two independent symptoms at once, but I have been wrong
three rounds running and I am not announcing a cause before testing it.

## Cheapest decisive test

Hand-issue `DRM_MSM_SUBMITQUEUE_NEW` (nr 0x07, `struct {u32 flags; u32 prio;}` →
returns `id`), then hand-issue `GEM_SUBMIT` with that `queueid`, using the
working `submit-probe.py` ioctl encoder. That is entirely free — no rebuild, no
flash — and it tests the queue hypothesis end to end:

- `SUBMITQUEUE_NEW` returns a valid `id` and a submit with that `queueid`
  moves `wptr_dw` ⇒ the driver is fine, freedreno is holding a broken queue.
- `SUBMITQUEUE_NEW` fails ⇒ the CAF driver's submitqueue implementation is
  incomplete, and that is the root cause, in the kernel, fixable in a kernel
  image.

Both outcomes are decisive and neither costs anything but a harness edit.

## Note on harness behaviour

The process still segfaults at teardown (exit 139) with no kernel oops. That
was present before any rendering too, so it stays a separate defect — though
if the submitqueue hypothesis is right it becomes the *same* bug seen twice.

Also: `texture bound, err=0x0500` (`GL_INVALID_ENUM`) — the `glTexImage2D`
call failed on its own terms. Unrelated to the submit question, but it means
TEST A's textured draw was sampling an incomplete texture. The compile result
is still valid, since that is what the test was for.