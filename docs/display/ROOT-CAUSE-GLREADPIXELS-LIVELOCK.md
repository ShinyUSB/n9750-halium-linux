# `glReadPixels` livelock — ROOT CAUSE FOUND: unbounded VBIF halt spin on a GBIF part

This is the `pm_runtime` livelock that `RESUME.md` describes as "the only
remaining wall" and that has been degrading every session (a kworker spinning
in `pm_runtime_work` since ~372 s, RCU stalls every ~63 s, loadavg 20, D-state
pileups, ~30-minute healthy window per boot).

## Direct observation on the live device (kernel #53)

No tracing needed — the state is visible in sysfs:

```
$ cat /sys/devices/platform/soc/2c00000.qcom,kgsl-3d0/power/runtime_status
suspending

t1: suspending active=1078944 susp=107514
t2: suspending active=1083957 susp=107514     <- 5 s later
t3: suspending active=1088614 susp=107514     <- 5 s later
```

`runtime_active_time` climbs ~1000 ms per second of wall clock while
`runtime_suspended_time` never moves. `runtime_status` is pinned at
**`suspending`**, which means `__pm_runtime_suspend()` has entered the device's
`.runtime_suspend` callback and **never returned**. The 66 ms autosuspend timer
keeps re-firing, so `pm_runtime_work` spins forever. That is exactly the
"RCU stalls every ~63 s, always `pm_runtime_work`, always the task in state R"
signature in `RESUME.md` — a livelock, not a deadlock, which is why there is no
crash record.

## The spin: `a6xx_pm_suspend()` halts VBIF on a part that has no VBIF

`drivers/gpu/drm/msm/adreno/a6xx_gpu.c:2552-2557`:

```c
/* Clear the VBIF pipe before shutting down */
/* FIXME: This accesses the GPU - do we need to make sure it is on? */
gpu_write(gpu, REG_A6XX_VBIF_XIN_HALT_CTRL0, 0xf);
spin_until((gpu_read(gpu, REG_A6XX_VBIF_XIN_HALT_CTRL1) & 0xf) == 0xf);   // <-- UNBOUNDED
gpu_write(gpu, REG_A6XX_VBIF_XIN_HALT_CTRL0, 0);
```

`spin_until()` is `while (!cond) cpu_relax();` — **no timeout, no `might_sleep`,
no error path**. If `VBIF_XIN_HALT_CTRL1` never reflects the requested halt, this
spins for the lifetime of the machine.

And the in-tree comment right above it is the tell: *"FIXME: This accesses the
GPU - do we need to make sure it is on?"* Nobody ever resolved it.

## Why it spins: this is a GBIF part, and upstream guards it

Upstream `a6xx_gpu.c:2170-2185` has the same three lines — inside
`a6xx_bus_clear_pending_transactions()` — **but gated**:

```c
} else if (!a6xx_has_gbif(adreno_gpu)) {
        gpu_write(gpu, REG_A6XX_VBIF_XIN_HALT_CTRL0, VBIF_XIN_HALT_CTRL0_MASK);
        spin_until((gpu_read(gpu, REG_A6XX_VBIF_XIN_HALT_CTRL1) &
                (VBIF_XIN_HALT_CTRL0_MASK)) == VBIF_XIN_HALT_CTRL0_MASK);
        gpu_write(gpu, REG_A6XX_VBIF_XIN_HALT_CTRL0, 0);
        return;
}
/* GBIF parts take the halt path below instead */
```

And upstream's own predicate (`a6xx_gpu.h:245-251`):

```c
static inline bool a6xx_has_gbif(struct adreno_gpu *gpu)
{
        if(adreno_is_a630(gpu))
                return false;
        return true;
}
```

**An a640 is a GBIF part** (`adreno_is_a640_family()` →
`ADRENO_6XX_GEN2`, and `adreno_is_a630()` is false for it). So upstream takes
the *else* branch and never touches `VBIF_XIN_HALT_CTRL0`. Our tree has no such
guard: it executes the VBIF halt unconditionally, on hardware that has no
VBIF, and waits for an acknowledgement from a block that is not there.

This is the same class of error as the `GBIF_HALT` finding in image 244 — an
a630-era sequence applied to an a640 — but this one runs on **every autosuspend**,
which is why it has been present the whole time and mistaken for general
instability. The corpus explicitly recorded `VBIF_XIN_HALT_CTRL0` as "written
by our tree, not found in CAF" (CAF only references it in a register-list
*definition* table at `adreno_a6xx.c:2513-2516`, never writes it in the suspend
path).

## Why `glReadPixels` specifically triggers it

`RESUME.md`: `glClear`, `glFlush` and `glFinish` all survive; only
`glReadPixels` — the first call that must *wait* for the CP — resets the
system. That fits exactly:

- A render-only path submits work and returns; the GPU stays referenced and
  autosuspend does not run.
- `glReadPixels` blocks on a fence, which requires a resume, and the
  autosuspend/resume cycle around it lands the device in the spin.
- Once `a6xx_pm_suspend()` is stuck in `spin_until()`, the system degrades in
  exactly the described way: RCU stalls, D-state pileups, ssh flakiness, loadavg
  climbing to 20 — and the phone survives, so no panic, so no pstore.

So this was never an independent second problem. It is the same missing
GBIF-vs-VBIF guard, and it has plausibly been interfering with *every* GPU
session including the earlier bring-up work.

## Fix

Guard the VBIF halt on GBIF parts, matching upstream:

```c
if (!adreno_is_a640(adreno_gpu) && !adreno_is_a640_family(adreno_gpu)) {
        /* legacy VBIF parts only; a640 is a GBIF part */
        gpu_write(gpu, REG_A6XX_VBIF_XIN_HALT_CTRL0, 0xf);
        spin_until(...);
        gpu_write(gpu, REG_A6XX_VBIF_XIN_HALT_CTRL0, 0);
}
```

and additionally bound the spin with `gpu_poll_timeout()` rather than
`spin_until()`, so no future register-availability surprise can wedge the
system again. Both changes are belt-and-braces: the guard fixes it, the
timeout makes the next such bug survivable.

Runtime-toggleable via a module parameter so it can be A/B'd on the live device
without a reflash, consistent with the other 243/244 toggles.

## Predicted readout

- `runtime_status` transitions to `suspended` within ~100 ms of idle, and
  `runtime_suspended_time` starts climbing.
- `glReadPixels` no longer wedges; the pm_runtime livelock and its RCU stalls
  disappear.
- Loadavg stays flat; the ~30-minute session window opens up.
- The render target is *still* all zeros — this is a stability fix, not a
  rendering fix. `BUFFER-IDENTITY-PROVEN.md` established the read is sound, so
  the render failure remains a separate open item (zap / secure mode).