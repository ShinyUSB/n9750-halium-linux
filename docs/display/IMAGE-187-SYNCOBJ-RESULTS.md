# Image 187 — DRIVER_SYNCOBJ landed and is genuinely working, but freedreno still never submits

Kernel `#129`. Image 186 found the root cause (`msm_driver` never set
`DRIVER_SYNCOBJ`, record `ROOT-CAUSE-NO-SUBMIT.md`); 187 added the bit. This
records what that actually bought us, which is *not* first pixels.

## The fix works — verified three independent ways

1. **Before/after on the ioctl itself.** `DRM_IOCTL_SYNCOBJ_CREATE` is gated at
   `drm_syncobj.c:499` by `drm_core_check_feature(dev, DRIVER_SYNCOBJ)`. Pre-187
   it returned `-EOPNOTSUPP`; on `#129` it returns **rc=0**.
2. **Confirmed live via a hand-built request.** `SYNCOBJ_CREATE` rc=0 from the
   `submit1.py` probe on `/dev/dri/renderD128`.
3. **The submit path itself now answers.** `msm_ioctl_gem_submit` is reachable
   and validates its input — a hand-built `GEM_SUBMIT` gets as far as
   `submit_bo()`/`submit_lookup_objects()` and returns **EINVAL** on my
   deliberately-malformed BO flags, which is the driver *processing* the call.

## But freedreno still issues zero GEM_SUBMIT

Re-traced on `#129` with the real-draw `egl8.py`:

```
SYNCOBJ ioctls:    0
MSM_GEM_SUBMIT:    0
```

And the ring is still frozen at the exact same place as before the fix:

```
CPDUMP[auto-post] sw : rb_iova=0x1000 rb_size=32768 wptr_dw=99 next_dw=99
CPDUMP[auto-post] hw : RB_RPTR=0x63 RB_WPTR=0x63
CPDUMP[auto-post] irq: INT_0_STATUS=0x0 INT_0_MASK=0x3d0e242 irqs=0
```

`wptr_dw=99`, `RPTR == WPTR == 0x63` — unchanged from `#128`. Nothing was
submitted before the fix and nothing is submitted after it. Freedreno is still
completely silent under `TU_DEBUG=bail,sched,sync,ioctl`.

## Honest assessment: DRIVER_SYNCOBJ was necessary, not sufficient

I predicted this would "move the wedge, not end it". It did something more
instructive: it proved syncobj was **one** blocker among several, and that
freedreno has a second, independent reason not to submit.

So the question I thought 187 would answer — "is it userspace or the driver?"
— is now answered *more sharply*: **the driver's submit path works** (it
accepts and validates a hand-built submit). Therefore whatever is stopping
freedreno is **in userspace**, and it is not the syncobj capability.

Also worth noting: `DRIVER_SYNCOBJ` did not cause a crash, a hang, or any
regression. The survivability work held. That is real evidence the one-line
change is safe to keep.

## A second, independent discovery: Mesa falls back to llvmpipe

While probing I hit something that changes how to read all of the earlier
evidence. On `#129` **before** the GPU is loaded:

```
GL_RENDERER = b'llvmpipe (LLVM 15.0.6, 128 bits)'
GL_NUM_EXTENSIONS = 144
```

Mesa silently uses **software rasterization** when `priv->gpu` is NULL, because
this kernel attaches the GPU on demand. And `MSM_GET_PARAM` returns
**errno=6 (ENXIO)** in that state, which is correct — `msm_ioctl_get_param`
returns `-ENXIO` when `!gpu`.

This is a trap I nearly fell into: any GL test run before the on-demand trigger
reports "llvmpipe" and looks like the GPU failed, when it means the opposite.
Every earlier `GL_RENDERER=FD640` reading was taken *after* a successful
trigger, so those stand — but the llvmpipe path is now a known confounder and
is recorded here so nobody re-discovers it.

## The ioctl-number saga, third attempt — now actually verified

I got DRM ioctl numbers wrong **three times** in this session, and each time
the errno looked like a meaningful driver refusal when it was just a malformed
request. Recording it properly this time, because the pattern is the lesson:

| attempt | number used | errno | what it actually was |
|---|---|---|---|
| 1 | `0x186643` | 2 ENOENT | not a real DRM request at all |
| 2 | `0xc01066a4` | 13 EACCES | collided with a generic legacy DRM ioctl |
| 3 | `0xc0106602` | 13 EACCES | **same collision** — I'd "removed" the base wrongly |
| 4 | `0xc0106642` | **0 ok** | correct: nr = `DRM_COMMAND_BASE + cmd` |

The resolution that actually stuck was to stop deriving numbers from macros and
instead **assert two known-good values** in the probe:

```python
assert _iorw_nr(0x40 + 0x00, 16) == 0xC0106640, "GET_PARAM encoding drifted"
assert _iorw_nr(0x00, 16) == 0xC0106600, "SYNCOBJ encoding drifted"
```

`MSM_GET_PARAM 0xc0106640` returned real driver values (`CHIP_ID=0x6040001`,
core=6), and `SYNCOBJ_CREATE 0xc0106600` returns rc=0. Both are now regression
tests in `harness/submit-probe.py`. Two lessons for the harness code generally:
**`ctypes.CDLL(..., use_errno=True)` is mandatory** or `get_errno()` returns 0,
and **ENXIO from an MSM ioctl means "GPU not loaded yet"**, not a failure.

## Where the remaining blocker lives

With the driver's submit path proven good, the remaining suspects are all in
Mesa/freedreno, and none of them can be tested without reading freedreno's
logic — which we don't have on the device (only the shipped `.so`). The
candidates, in the order I'd rank them:

1. **A capability or chip-ID mismatch that makes freedreno treat this as an
   unsupported/degraded GPU.** It reports `FD640` correctly, but `MAX_FREQ`
   comes back as `0x283baec0` — nonsense, because `adreno_get_param` returns
   `adreno_gpu->base.fast_rate`, which is unpopulated. A driver reporting an
   absurd max frequency is exactly the sort of thing that makes userspace pick
   a conservative or fallback path. **This is my leading suspect and it is
   cheap to test** — populate `base.fast_rate` from the DT operating-points
   table and see if freedreno starts submitting.
2. **UBWC / cache-mode parameters.** `HIGHEST_BANK_BIT=0xf` is hardcoded from
   the vendor DT (image 167), and `MSM_BO_UBWC_*` bits drive freedreno's
   tile/decompress decisions. A wrong value here can suppress the flush path.
3. **Missing `MSM_PARAM_GMEM_SIZE` semantics** — returns `0x100000` (1 MB),
   which may be below what freedreno wants for this part.

Option 1 is the clear next move and is a small, well-understood kernel change.
It also has the virtue of being a genuine bug: `MAX_FREQ` is currently garbage
and should not be.

## State

Phone alive, `#129`, GPU loaded (`ALL 10 PASS`), still reachable over USB RNDIS.
Bring-up lottery won on the first ticket after flash. `wptr_dw` remains the one
number to watch: the moment it stops being 99, a submit has happened.