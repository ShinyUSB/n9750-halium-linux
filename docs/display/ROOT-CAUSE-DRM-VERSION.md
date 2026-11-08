# 🎯🎯 ROOT CAUSE #3 — the driver reports DRM version 1.2, so Mesa 22.3 takes the *legacy* submit path, whose funcs table has no `.flush`

Found on `#131`, 2026-10-09. Three independent lines of evidence converge, and
the fix is **one number in the kernel**.

## The mechanism

`src/freedreno/drm/msm/msm_pipe.c:255`:

```c
if (fd_device_version(dev) >= FD_VERSION_SOFTPIN)
        pipe->funcs = &sp_funcs;
else
        pipe->funcs = &legacy_funcs;
```

and `src/freedreno/drm/msm/msm_device.c:63`:

```c
dev->version = version->version_minor;   /* straight from DRM_IOCTL_VERSION */
```

So the choice of submit path is made purely from the version number the kernel
reports. Compare the two tables (`msm_pipe.c:207` and `:217`):

```c
static const struct fd_pipe_funcs sp_funcs = {
        .ringbuffer_new_object = fd_ringbuffer_sp_new_object,
        .submit_new             = msm_submit_sp_new,
        .flush                  = fd_pipe_sp_flush,   /* <-- present */
        ...
};

static const struct fd_pipe_funcs legacy_funcs = {
        .ringbuffer_new_object = msm_ringbuffer_new_object,
        .submit_new             = msm_submit_new,
        /* NO .flush — left NULL */
        ...
};
```

Mesa 22.3's own `DRM_IOCTL_VERSION` on this device returns:

```
version_major = 0    version_minor = 0    version_patchlevel = 0
name = ''            driver = ''
```

**Zero.** So `fd_device_version(dev) < FD_VERSION_SOFTPIN`, Mesa takes
`legacy_funcs`, and the legacy submit path in this CAF-era driver ends in a
**NULL function-pointer call**.

## The smoking gun: `pc : 0x0`, `BLR x4`

The crash, straight from the kernel:

```
python3:gdrv0[1889]: unhandled level 2 translation fault (11) at 0x00000000,
                     esr 0x82000006, in python3.11
pc : 0x0
lr : 0x7faf2bb54c
```

Resolving `lr` against the live mapping of `msm_dri.so` (base `0x7fae8e0000`)
and disassembling gives:

```
0x9db544:  f9000265   STR x5, [x19]
0x9db548:  d63f0080   BLR x4        <== CALL SITE
```

**`BLR x4` with `x4 == 0`** — an indirect call through a NULL function pointer,
inside the Mesa driver. That is exactly the shape of a missing `.flush`.

## Proof it is the flush path, not teardown

A staged harness (`harness/stagetest.py`) builds up the GL sequence and counts
kernel faults per stage:

| stage | what it does | new faults |
|---|---|---|
| S1 | load libEGL | **0** |
| S2 | + GBM display, `eglInitialize` | **0** |
| S3 | + create context, `makeCurrent` | **0** |
| S4 | + FBO | **0** |
| S5 | + shader compile/link | **0** |
| S6 | + `glClear` + `glFlush` | **1** |

Only the stage that actually **flushes** crashes. That eliminates teardown
entirely — which also explains why the old "SIGSEGV at teardown" reads were
misleading: the fault is asynchronous on Mesa's driver thread, so it lands
wherever the main thread happens to be.

## And it explains the missing `defer:` line

With `FD_MESA_DEBUG=msgs` the driver prints up to
`fd_batch_resource_write` and then dies. We never see `defer: %u` from
`fd_submit_sp_flush`. On the legacy path that message is never reached —
consistent with the NULL call happening upstream of it.

## The fix

`drivers/gpu/drm/msm/msm_drv.c:61`:

```c
#define MSM_VERSION_MAJOR    1
#define MSM_VERSION_MINOR    2      /* <-- too old */
#define MSM_VERSION_PATCHLEVEL 0
```

Bump the advertised version so `version_minor >= FD_VERSION_SOFTPIN`, and Mesa
selects `sp_funcs`. This is exactly what upstream did — the `FD_VERSION_*`
enum exists precisely so drivers can advertise which submit path they support.

**Caveat, stated plainly:** I have not yet read `FD_VERSION_SOFTPIN`'s numeric
value (it lives in a header I have not opened), so the exact number to set is
not yet confirmed. That is the one loose end before flashing.

Also worth flagging: our CAF driver *does* implement `MSM_SUBMITQUEUE_NEW`
(nr `0x0A`), and its handler is a stub that returns `id = 1`. So the sp path
has the minimum it needs. But a stubbed submitqueue is exactly the kind of
thing that must be tested rather than assumed, and the honest expectation is
that this unblocks the submit and may expose a *next* bug rather than pixels
immediately.

## Status of the three root causes

1. `msm_driver` never set `DRIVER_SYNCOBJ` (image 187) — real, fixed, though
   on reflection irrelevant to the Gallium path.
2. `msm_gpu_submit()` deadlocked in `rpm_resume` (image 189) — real, fixed,
   and **necessary**: without it no submit could ever complete.
3. **Advertised DRM version 1.2 pushes Mesa onto a legacy submit path that
   NULL-derefs on flush.** This is the one actually in our way right now.

Phone alive on `#131`, GPU loaded, reachable over USB RNDIS.