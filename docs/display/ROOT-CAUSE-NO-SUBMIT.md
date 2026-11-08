# ROOT CAUSE FOUND — msm never advertises `DRIVER_SYNCOBJ`, so freedreno never submits

Found on kernel `#128` (image 186), no new flash required. This is the answer
to "why does nothing get submitted", and it has been in front of us the whole
time in `msm_drv.c`.

## The defect

```c
/* drivers/gpu/drm/msm/msm_drv.c:2026 */
static struct drm_driver msm_driver = {
	.driver_features    = DRIVER_HAVE_IRQ |
				DRIVER_GEM |
				DRIVER_PRIME |
				DRIVER_RENDER |
				DRIVER_ATOMIC |
				DRIVER_MODESET,      /* <-- no DRIVER_SYNCOBJ */
```

`grep -rn DRIVER_SYNCOBJ` across the whole tree returns exactly two drivers —
`i915` and `amdgpu`. **This CAF msm driver never sets it.**

## Why that means freedreno never submits

`drivers/gpu/drm/drm_syncobj.c:499`:

```c
static int drm_syncobj_create_ioctl(struct drm_device *dev, void *data, ...)
{
	if (!drm_core_check_feature(dev, DRIVER_SYNCOBJ))
		return -EOPNOTSUPP;
```

and `drm_ioctl.c:246` reports it to userspace:

```c
	case DRM_CAP_SYNCOBJ:
		req->value = drm_core_check_feature(dev, DRIVER_SYNCOBJ);
		return 0;
```

So userspace asks "do you support syncobj?", gets 0, and Mesa's `kms_dri` DRI2
backend concludes it cannot build a fence chain. Freedreno needs a syncobj for
its per-batch fence: `fence_batch()` in `tu_render_manager` creates one via
`drm_syncobj_create`, and when that is unavailable it has nowhere to signal
completion — so `tu_flush` never gets as far as issuing `DRM_IOCTL_MSM_GEM_SUBMIT`.

That is exactly the observed signature:

- **Zero `SYNCOBJ*` ioctls** in the strace, and zero `MSM_GEM_SUBMIT`.
- 76 `MSM_GEM_INFO` / 39 `MSM_GEM_NEW` — Mesa allocates buffers fine, then stops.
- Freedreno logs **nothing at all** under `TU_DEBUG=bail,sync,sched,ioctl` —
  consistent with a capability that silently disables a path, not a fault.
- `glClear` and `glDrawArrays` return `glGetError()==0` — freedreno records the
  work into a command stream it never flushes.
- `glFinish` then blocks (or segfaults, on later runs) waiting for a fence that
  will never be signalled.

## This also retires the entire IRQ investigation

There was never any GPU work for the core interrupt to complete. `wptr_dw=99`
and `CP_RB_RPTR == CP_RB_WPTR == 0x63` are not a stalled command processor —
they are a ring nobody wrote to. The CP was healthy the whole time, which is why
its perf counter advanced ~35-40M per 50 ms in every dump.

Also confirmed along the way: `INT_0_MASK = 0x03D0E242` decodes bit-exact to
`A6XX_INT_MASK` including `CP_CACHE_FLUSH_TS` (bit 20). So no unmask fix was
ever available, and the consultants' threaded-IRQ idea was never the issue.

## Device identity, now read directly from the driver

With a corrected `MSM_GET_PARAM` (see the errno saga below), on pipe 3D0:

| param | value | meaning |
|---|---|---|
| `GPU_ID` | `0x280` | revn 2.8.0 |
| `CHIP_ID` | `0x6040001` | patchid=1, minor=0, major=0x40, **core=0x06 (Adreno 6xx)** |
| `GMEM_SIZE` | `0x100000` | 1 MB |
| `MAX_FREQ` | `0x283baec0` | nonsense — `base.fast_rate` unpopulated |
| `PRIORITIES` | `3` | |
| `FAULTS` | `0` | **zero GPU faults observed** |
| `SUSPENDS` | `0` | |
| `HIGHEST_BANK_BIT` | `0xf` | 15 |

`CHIP_ID` core=6 confirms the driver correctly identifies this as a 6xx part,
and `GL_RENDERER=FD640` is right. `FAULTS=0` is worth noting: the driver has
never recorded a single GPU fault, which independently corroborates that nothing
was ever executed.

## Two process notes

**I misread an errno as a driver refusal.** My first `GET_PARAM` probe used a
badly hand-computed ioctl number and got `errno=2` (ENOENT) for every call; I
briefly took that as "the driver refuses these params". Second attempt was also
wrong and returned `errno=13` (EACCES), which I nearly recorded as a DRM_AUTH
permissions problem. Both were just malformed request numbers — I was folding
`DRM_COMMAND_BASE` into the `_IOC` nr field twice. Computed properly
(`dir=3`, `type='f'=0x66`, `nr=0x40+cmd`, `size=sizeof(struct)`) it works
first try. **Lesson: never infer driver behaviour from a hand-rolled ioctl's
errno before the ioctl number itself is verified.**

**`libc = ctypes.CDLL("libc.so.6")` without `use_errno=True`** makes
`ctypes.get_errno()` always return 0. That turned the first run's real errno
into a meaningless `0`. Both are recorded in
`harness/egl-param-probe.py` comments.

## The fix

One line, in `msm_driver.driver_features`:

```c
	.driver_features = DRIVER_HAVE_IRQ |
				DRIVER_GEM |
				DRIVER_PRIME |
				DRIVER_RENDER |
				DRIVER_ATOMIC |
				DRIVER_MODESET |
				DRIVER_SYNCOBJ,      /* <-- add */
```

Worth knowing before we flash it:

- **This is the first real fix in the series.** Images 1-186 were all diagnostics
  and survival work; nothing so far changed whether the GPU could render.
- **This kernel predates what freedreno expects.** `DRIVER_SYNCOBJ` and the
  syncobj ioctls were added in 4.13; this is 4.14 with a CAF driver, so the
  plumbing is present (`drm_syncobj.c` exists) and only the feature bit is
  missing. That makes a one-line fix plausible — but it is not guaranteed to be
  sufficient, and I should not promise first pixels on the strength of one line.
- **Re-enabling syncobj lets fences actually be created**, which means
  `glFinish` will start waiting on a real `CACHE_FLUSH_TS` interrupt that has
  never once fired (IRQ count is still 0). So image 187 will very likely move
  the wedge rather than end it. That is progress — it is the first state in
  which the IRQ is genuinely load-bearing — but expect another cycle.
- Risk: enabling a capability the driver was never tested with could expose
  further gaps. Given the survivability work (no panic escalation, WDT
  disarmed, async trigger) the downside is bounded.

Recommendation: build 187 as the one-line `DRIVER_SYNCOBJ` change, keeping
everything else identical, and keep the auto-probe in place — its
`wptr_dw` reading is now the single best progress indicator we have, because it
tells us immediately whether a submit actually happened.