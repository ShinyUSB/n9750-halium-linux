# Image 207 — the last `pm_runtime` site in the bring-up path

Date: 2026-11-11. Built, packaged, **not yet flashed**. One call pair removed.

## Image 206 result: REGRESSION — but an informative one

206 changed `msm_gem_get_vaddr()` from `pgprot_writecombine()` to
`pgprot_noncached()`. The GPU stopped working:

| check | 205 | 206 |
|---|---|---|
| bring-up | `BABEFACE`, 7/7 ACKs, `hw_init ret=0` | same — **fine** |
| `CPDUMP[auto-rest]` | present (t=59) | **absent** |
| `CPDUMP[auto-post]` | present (t=71) | **absent** |
| `a6xx-load` thread | alive, ran to completion | **gone from `/proc`** |
| open `/dev/dri/renderD128` | OK | **blocks forever** |
| read `a6xx_gpu_cpstate` | OK | **blocks forever** |
| `GL_RENDERER` | `FD640` | **`llvmpipe (LLVM 15.0.6)`** |
| submits / IRQ 487 | 1 per submit | **0** |

**The most important line in that table is `GL_RENDERER`.** `glstage.py 5`
reported "PASS" on 206 — but Mesa had fallen back to *software rendering*. The
clear, flush and finish all completed on the CPU. Every green result was
hollow: zero submits, hangcheck timer never armed, IRQ 487 never incremented.

This is the second time a passing harness hid a dead GPU. `GL_RENDERER` is now
a required gate before any stage result is believed.

**Bring-up itself was never the problem** — `BABEFACE ms~1`, 7/7 HFI ACKs and
`HWINIT[20] adreno_hw_init ret=0` all appeared normally at t=161.4. The failure
is strictly *after* hw_init returns.

## Root cause

`adreno_load_gpu()` (`adreno/adreno_device.c:240,244`):

```c
pm_runtime_get_sync(&pdev->dev);
mutex_lock(&dev->struct_mutex);
ret = msm_gpu_hw_init(gpu);
mutex_unlock(&dev->struct_mutex);
pm_runtime_put_sync(&pdev->dev);
```

This is the **last remaining `pm_runtime` call in the GPU bring-up path**, and
the same bug images 184, 189, 203 and 204 fixed everywhere else. It was simply
never found, because each earlier fix was made at whatever site the current
symptom pointed to.

Why it is fatal here, exactly as recorded at `msm_gpu.c:557`:

> our GPU is powered by the on-demand sysfs trigger, which powers the
> hardware directly and does not drive the runtime-PM state machine.
> `pm_runtime_enable()` was still called at probe, so `pm_runtime_get_sync()`
> believes the device is suspended and tries to resume it — and CAF's resume
> path deadlocks.

And why it takes the *whole GPU* down rather than just erroring:

`load_gpu_real()` holds `init_lock` across the entire `adreno_load_gpu()`
call. `init_lock` is what every subsequent driver and EGL initialisation
blocks on. One stuck bring-up therefore does not produce an error — it makes
the GPU silently *not present*, and Mesa falls back to llvmpipe without saying
so at default log levels.

## Confidence, stated honestly

The chain is circumstantial, not a red-handed catch: the `a6xx-load` thread was
already gone from `/proc` by the time it was inspected, so `/proc/PID/stack`
could not confirm it was parked in `rpm_resume`. What *is* directly
established:

- hw_init completed normally, then no further thread output appeared
- neither post-hw_init branch of `load_gpu_thread()` ran (no `CPDUMP`, no
  "produced no GPU")
- every consumer of the driver blocks on a mutex, in **S** state, not **D**

The single unremoved `pm_runtime` call in that exact window is the obvious
culprit, and removing it is a no-risk change either way. But it is inference,
not proof, and 207's results will confirm or refute it.

## The fix

```c
-		pm_runtime_get_sync(&pdev->dev);
 		mutex_lock(&dev->struct_mutex);
 		ret = msm_gpu_hw_init(gpu);
 		mutex_unlock(&dev->struct_mutex);
-		pm_runtime_put_sync(&pdev->dev);
```

Safe on the same grounds as the other four removals: the GPU is already powered
and clocked by the trigger, there is no matching `put` whose loss matters, and
`hw_init` only needs the hardware up.

Note this image **keeps** the 206 coherency fix. 206's hypothesis was never
actually tested — the GPU was absent, so no submit ever reached the ring. 207
is the image that finally tests it.

## Decision rule, declared BEFORE flashing

| observation | conclusion |
|---|---|
| `GL_RENDERER` = `FD640` again **and** `CPDUMP[auto-rest]` appears | `pm_runtime` was it; GPU restored, coherency fix now testable |
| after a submit, `CP_SCRATCH_REG2` ≠ 0 | **the GPU executes real commands** — 206's coherency fix was the last blocker |
| `fence` advances 0 → 1, hangcheck stops firing | fences retire; deadlock-and-storm class fully closed |
| `glstage 6` (`glReadPixels`) returns correct pixels | **goal reached — a real GPU producing pixels** |
| `GL_RENDERER` = `llvmpipe` again | something else still blocks bring-up; re-run the `/proc` sweep, but earlier this time |

## Verification

- Build exit 0, no new warnings.
- Byte-identical to 206 in header (all 10 fields), cmdline, ramdisk, size.

```
artifacts/flash/207-halium-boot-a6xx-coherent-nopm.img
sha256 d3a814a4b853d3b6e111860ac16505d33579eb1cd4f44a4ab0ecfd908baca0bc
```

## Scoreboard

| image | fix | result |
|---|---|---|
| 202 | GMU firmware coherency | bring-up 6-for-6, first reliable GPU |
| 203 | missing `a6xx_last_fence` NULL deref | oops eliminated |
| 204 | recover-path `pm_runtime` deadlock | render node survives, glstage 5/5 |
| 205 | recover no longer power-cycles GPU | instrument survived; GPU proven to consume the ring |
| 206 | kernel GEM mappings coherent | **regression** — hit the last `pm_runtime` site |
| **207** | **remove that site** | **expect `FD640` back, coherency fix testable** |

## Not changed

Images 187–206 untouched. 202–205 all proven and stay. 206's coherency fix is
kept deliberately — it is untested, not disproven.
