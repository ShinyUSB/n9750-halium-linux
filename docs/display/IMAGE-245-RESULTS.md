# Image 245 — the reframe: most of this project's "rendering is broken" evidence was a suspend artifact

Kernel #55 (image `245-halium-boot-a6xx-hwcgvfy.img`,
sha256 `e949e89511809beac22466c8d2f83ec4779cd7974908865f3a7fe9d25700c2e1`).

Image 245 is **diagnostic only**. The sole functional change over 244 is a
readback-verification printout in `a6xx_set_hwcg()`. The cosmetic VBIF guard
proposed earlier was **reverted at the user's request** and is not in this image.

## 1. The pm_runtime "livelock" was a wedged BLOCKED suspend, not a spin

My first diagnosis (retracted, see `RETRACTION-VBIF-LIVELOCK.md`) claimed an
unbounded `spin_until()`. Wrong: `spin_until` is locally redefined at
`adreno_gpu.h:178` and bounded to 1000 ms.

What the sysfs data actually showed:

```
runtime_status = suspending          <- entered the callback, never returned
active_time   climbing ~1000 ms/s
suspended_time frozen
```

And the decisive measurement I initially botched (twice, on wrong awk field
offsets — per-CPU `idle` is `$5`, not what I used): sampling `/proc/<pid>/stat`
across every task shows **every CPU ~100% idle**, the top consumer being my own
`ssh bash`. So nothing was spinning. A blocked-task sweep found the culprit:

```
kworker/0:1  D      <- uninterruptible sleep, i.e. BLOCKED in kernel
```

`a6xx_pm_suspend()` blocks somewhere other than `spin_until`, never returns, and
the 66 ms autosuspend re-fires forever. **The exact call site is still not
established** (every `spin_until` on that path is bounded). Candidates not yet
examined: `a6xx_gmu_stop()`, a QMP/mailbox/cmd-db wait, or an RPMh/SCM call in
the CAF suspend tail.

### Why it matters enormously: the GPU was half torn down the whole time

`a6xx_pm_suspend()` disables some domains and then hangs, so the device sits
permanently at `runtime_status=suspending` — **partially powered down**. The
measurable consequences:

- CP domain still clocked → ring drains, fences signal "msm A640 signalled",
  `CP_TIMESTAMP clocking 1`. Everything CP-side looks perfectly healthy.
- RB / SP / UCHE domains already gated off → **every draw is silently
  discarded**, with no fault and no interrupt.

That is precisely the signature this project has been chasing since image 235.
So:

> **Every "the GPU does not write render targets" result from images 235–244 was
> measured on a half-suspended GPU.** Those conclusions about rendering are not
> trustworthy and should be treated as void pending re-testing on a healthy GPU.

Corroboration: `power/control` writes block (pm_runtime serializes behind the
stuck suspend), and `/sys/kernel/debug/dri/0/rd` returns `EBUSY` — so for much
of this project there was **no way to read a single GPU register**. Every
register claim in the corpus was made without register access.

### The workaround (no reflash)

Pin runtime PM active **before the GPU is ever used**:

```sh
echo on > /sys/devices/platform/soc/2c00000.qcom,kgsl-3d0/power/control
```

On a fresh boot, before any GPU activity, this succeeds and the device never
enters `RPM_SUSPEND`. Loadavg stays flat over 20 s idle; no wedge. It must be
done *before* first GPU use — afterwards `pm_runtime_resume` blocks behind the
stuck suspend and the shell hangs.

## 2. Image 245's actual job: verify the clock gates instead of assuming them

The in-tree comment on the existing probe said, in so many words:
*"We have never once shown those blocks are clocked."* Since `gpu_read()` of an
unimplemented address returns Adreno's `0xdeadbead` signature, reading back
every entry of CAF's `a640_hwcg[]` table and comparing against CAF's expected
value distinguishes three cases that previously looked identical:

- reads back CAF's value → address correct, write landed
- reads `0xdeadbead` → **address wrong for a640**, write went nowhere
- reads a different value → write did not stick

Added to `a6xx_set_hwcg()`, after the table is written, when `state` is true.

### Result: 43/46 exact, zero deadbead

```
HWCG244: a640 table, 46 entries, state=1 CNTL_TEX_FCHE=0x2
HWCGVFY: MISMATCH off=0x0100 want=0x00040f00 got=0x00000f00
HWCGVFY: MISMATCH off=0x0120 want=0x00000222 got=0x00000002
HWCGVFY: MISMATCH off=0x0121 want=0x00000111 got=0x00000001
HWCGVFY: 3/46 entries did not read back as CAF specifies
```

**No `0xdeadbead` at any of the 46 addresses.** Every register offset in the
table is valid on a640 — the "we wrote into the void" hypothesis is dead.

The 3 deviations are all TEX_FCHE / CCU-hysteresis timing bits the hardware
does not reflect, and **none can explain the failure**: the failing draws are
solid-colour with no textures involved, and `glClear` alone already reads back
zero.

### "Unclocked render block" is refuted

Independent confirmation from the older probe, whose values also match CAF's
a640 table exactly:

| register | CAF a640 | readback |
|---|---|---|
| `CLOCK_CNTL2_RB0` | `0x01002222` | `0x01002222` ✓ |
| `CLOCK_CNTL2_SP0` | `0x02222220` | `0x02222220` ✓ |
| `CLOCK_CNTL2_TP0` | `0x22222222` | `0x22222222` ✓ |
| `CLOCK_CNTL2_RAC` | `0x00005555` | `0x00005555` ✓ |
| `CLOCK_CNTL_SP0` | `0x02222222` | `0x02222222` ✓ |

`CLOCK_CNTL_RB0`, `CLOCK_CNTL_CCU0` and `CLOCK_CNTL_UCHE` also verify. Note
`CLOCK_CNTL2_UCHE = 0xdeadbead` is **expected and correct** — CAF gates UCHE via
`CLOCK_CNTL_UCHE`, not `2_UCHE`.

## 3. With PM pinned, we finally have a genuinely healthy GPU — and it still writes nothing

This is the first real test of rendering in the project's history.

`glReadPixels` **works** on image 245 with PM pinned (it never did before):
`readpixels err=0x0000`, returning `PIXEL=[0,0,0,0]`.

Measured state across **11,683 samples** written to disk by a userspace poller
(`/root/poller.sh` → `/root/cpsamples.txt`; printk-based dumps die with the
freeze, disk-written samples survive — the image-196 design intent, finally used):

| observation | value | meaning |
|---|---|---|
| `SPTPRAC_PWR_CLK_STATUS` | `0x00000000` (11683/11683) | GX fully powered **ON** |
| `RPMH_POWER_STATE` | `0x00000000` (11688/11688) | not in a low-power state |
| `CP_RB_WPTR` / `CP_RB_RPTR` | equal (`0xae`) | ring fully drained |
| `CP_TIMESTAMP` | `clocking 1` on every sample | CP clock never stopped |
| `CP_INTERRUPT_STATUS` | `0x00000000`, always | **zero faults, ever** |
| `RBBM_STATUS` | `0x1` idle / `0x00e595e5` busy×12 | busy only during real work |
| `GMU2HOST_INTR_INFO` | `0` / `0x80000000` | no GMU error |
| `glReadPixels` | `PIXEL=[0,0,0,0]` | not even the clear's alpha |

Fully powered, executing, draining, fault-free — and producing nothing.

### A compute shader fails identically (does not use the render block)

New harness `/usr/local/bin/wrtest.py` (ctypes, no compiler needed on device;
no `gcc` present). `drawprobe.py` is FBO+readback only. `wrtest.py` adds:

| test | path exercised | result |
|---|---|---|
| T1 compute → SSBO | shader writes **buffer** memory, no RB | **all zeros**, no GL error |
| T2 transform feedback | vertex shader → buffer | `GL_INVALID_OPERATION`, heap garbage — **unsupported, proves nothing** |
| T3 fragment → FBO | render path | `[0,0,0,0]` |
| T4 `GL_TIME_ELAPSED` | did GPU report work | inconclusive (harness bug, not fixed) |

T1 matters most: compute bypasses the render block entirely and **still writes
nothing**. So this is not RB-specific.

(Note: T2's buffer initially looked like a "GPU wrote memory" success — it was
heap pointers, `0x7f…`, and my `-0.0` nonzero test was wrong. Corrected to raw
byte comparison. Do not resurrect the T2 claim.)

## 4. New leading hypothesis: secure mode / zap

The evidence now partitions perfectly:

- **CP domain — works.** Stream built, submitted, retired, ring drained, fences
  signal, clocking, no faults.
- **Shader domain — completely silent.** No pixels, no SSBO writes, **no fault
  bit, no interrupt, no GMU error.**

A compute shader failing the same way as rendering is the strongest single
signal, because compute shares the SP/UCHE shader units while bypassing the RB
entirely. Nothing that is wrong with clocks, addresses, the RB, or the MMU
explains "shader units silently do nothing while the CP domain is perfect".

Zap firmware / secure mode explains exactly this asymmetry: with no zap loaded,
shader-unit code fetch/execution is refused by hardware policy, silently.

The zap firmware is already secured on the host
(`/home/usb/mnt-sda/a6xx-build/fw/gpu-fw/a640_zap.{mdt,b00,b01,b02}`, extracted
from the Samsung-signed `apnhlos` image — see `GPU-ZAP-FIRMWARE.md`). The
obstacle is boot-critical DT work: see `GPU-ZAP-FIRMWARE.md` and the next-steps
list in `RESUME.md`.

## 5. New hazard: a real draw can freeze the machine

On image 245 with PM pinned, one `drawprobe.py` run **hard-froze the phone**
(ssh died, manual reboot required). A later identical run completed normally.

- **Intermittent, not deterministic.**
- Recovered only by manual reboot; a hard freeze writes no pstore, so
  `/proc/last_kmsg` and dmesg are lost. The userspace poller writing to disk is
  the only instrument that survives.
- This is *progress of a kind*: with PM wedged in the old state, draws never
  reached hardware and so could not hang. Now they do execute, and a genuine
  execution path can hang.
- Every future experiment on image 245 should be launched **detached** (`nohup`)
  with the disk poller running, so a freeze still yields data.

## 6. Refuted / changed this round

- **Unclocked RB/SP/CCU/UCHE** — refuted, registers verified against CAF.
- **hwcg register addresses wrong for a640** — refuted, zero `0xdeadbead`.
- **`glReadPixels` livelock (as a CPU livelock)** — refuted; it is a blocked
  suspend; pinning `control=on` avoids it and `glReadPixels` then works.
- **Transform feedback as evidence** — withdrawn, unsupported here.
- **"the read path is broken"** — `glReadPixels` demonstrably works and returns
  a sound zero.
- Still open and now *prioritised*: exact call site of the suspend block;
  zap/secure mode; the intermittent draw-triggered freeze.