# Image 208 — keep the `get_sync`, drop only the `put_sync`

Date: 2026-11-11. Built, packaged, **not yet flashed**. One line removed (207 had
removed two).

## Image 207 result: REGRESSION, and it inverted the obvious fix

207 removed *both* `pm_runtime_get_sync()` and `pm_runtime_put_sync()` from
`adreno_load_gpu()`, reasoning that they were the last site of the bug that
images 184/189/203/204 fixed elsewhere.

The GPU stopped initialising at all:

```
A6XX_TRACE: enter msm_gpu_hw_init
A6XX_TRACE: msm_gpu_hw_init no-op (needs_hw_init==0)
```

| check | 206 (get+put) | 207 (neither) |
|---|---|---|
| hw_init | **ran** — `BABEFACE`, 7/7 ACKs, `ret=0` | **skipped** — `needs_hw_init==0` |
| `CPDUMP[auto-rest]` | absent | present, but **all zeros** |
| `rb_iova` / `RB_BASE` / `RB_CNTL` | — | `0x0` / `0x0` / `0x0` |
| HFI ACKs | 7 | **0** |

One genuine gain: removing the pair let `load_gpu_thread()` run to completion,
so `CPDUMP[auto-rest]` appeared again. That confirms the load path itself is
sound and it was genuinely the `pm_runtime` calls holding `init_lock`.

## Why 207 was wrong

`gpu->needs_hw_init = true` is assigned in exactly **two** places:

```
msm_gpu.c:176     msm_gpu_pm_runtime_resume()
a6xx_gpu.c:1539   a6xx_pm_resume()
```

Both are runtime-PM **resume** paths. So the `pm_runtime_get_sync()` in the load
path is not incidental overhead — it is the only thing that **arms the hardware
init**. Remove it and `msm_gpu_hw_init()` short-circuits on
`needs_hw_init==0`, the GPU is never programmed, and every consumer sees a
device that exists but does nothing.

This is why "remove the last `pm_runtime` like 184/189/203/204 did" was wrong:
those four sites were all `get_sync` calls in paths that did **not** need the
resume to arm anything. This one does. The pattern that worked four times
silently inverted on the fifth.

**Lesson: a proven remedy is not automatically transferable. What made 184/189/
203/204 correct was never "remove pm_runtime" — it was "remove pm_runtime *where
it wedges*". Nobody had checked which half wedges here.**

## What actually wedged on 206

Not the `get` — the **`put`**.

- On 206 the `get` demonstrably worked: it drove the runtime resume that set
  `needs_hw_init`, and `hw_init` then ran to completion (`BABEFACE` at
  t=161.4).
- The failure came *after* `hw_init` returned, which is exactly where
  `pm_runtime_put_sync()` sits.
- `pm_runtime_put_sync()` triggers a runtime **suspend**, and CAF's suspend path
  is the thing known to hang on this tree — images 179–181 recorded
  `pm_suspend` wedging inside `printk`/`console_unlock`, and the live evidence
  from image 204 was `2c00000.qcom,kgsl-3d0/power/runtime_status` reading
  `suspending`, i.e. stuck mid-transition.

## The fix

```c
 		pm_runtime_get_sync(&pdev->dev);   /* KEPT - arms needs_hw_init */
 		mutex_lock(&dev->struct_mutex);
 		ret = msm_gpu_hw_init(gpu);
 		mutex_unlock(&dev->struct_mutex);
-		pm_runtime_put_sync(&pdev->dev);    /* DROPPED - suspends, wedges */
```

Take the reference and never give it back. That matches this tree's stated
design — `msm_gpu_submit()`'s comment says *"our bring-up leaves it up"*, and
the GPU is powered on demand by the sysfs trigger and meant to stay up until
reboot. Leaving `usage_count` at +1 is correct for that model, and there is no
working suspend path to starve.

If a real suspend path is restored, this must return behind a live "is it
actually suspended" check.

This image **keeps** the 206 coherency fix (`pgprot_noncached` for kernel GEM
mappings), which is still untested rather than disproven.

## Decision rule, declared BEFORE flashing

| observation | conclusion |
|---|---|
| `GL_RENDERER` = `FD640` **and** `BABEFACE` + 7/7 ACKs | the `put` was the wedge; GPU restored and the coherency fix is finally testable |
| `CPDUMP[auto-rest]` shows **non-zero** `rb_iova`/`RB_BASE` | the auto-dump instrument is back |
| after a submit, `CP_SCRATCH_REG2` ≠ 0 | **the GPU executes real commands** — coherency was the last blocker |
| `fence` advances 0 → 1, hangcheck stops firing | fences retire; deadlock-and-storm class fully closed |
| `glstage 6` (`glReadPixels`) returns correct pixels | **goal reached — a real GPU producing pixels** |
| `needs_hw_init==0` again | the resume is not arming; investigate `POSTINIT-PM` / runtime_resume |

Gate order matters: **`GL_RENDERER` first, always.** A stage result on
`llvmpipe` means nothing.

## Verification

- Build exit 0, no new warnings.
- `adreno_device.c` now contains exactly one `pm_runtime` call: the `get`.
- Byte-identical to 207 in header (all 10 fields), cmdline, ramdisk, size.

```
artifacts/flash/208-halium-boot-a6xx-noput.img
sha256 2a16adb5fa1ece2c25158dbf4799459e88b6caad040f486bfd849e3d0dc6d6c3
```

## Scoreboard

| image | change | result |
|---|---|---|
| 202 | GMU firmware coherency | bring-up 6-for-6, first reliable GPU |
| 203 | missing `a6xx_last_fence` NULL deref | oops eliminated |
| 204 | recover-path `pm_runtime` deadlock | render node survives, glstage 5/5 |
| 205 | recover no longer power-cycles GPU | instrument survived; GPU proven to consume the ring |
| 206 | kernel GEM mappings coherent | regressed — wedged on the `put` |
| 207 | removed both `get`+`put` | regressed harder — `needs_hw_init` never armed |
| **208** | **`get` kept, `put` dropped** | **expect `FD640` back; coherency fix testable** |

## Not changed

Images 187–207 untouched. 202–205 all proven and stay. 206's coherency fix is
kept deliberately.
