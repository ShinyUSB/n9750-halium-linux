# Autosuspend audit — closed, negative; and the freeze explained

Date: 2026-10-20

## 1. The GPU is never runtime-suspended

Measured on the live device:

```
/sys/devices/platform/soc/2c00000.qcom,kgsl-3d0/power/
   runtime_status        = unsupported
   control               = auto
   autosuspend_delay_ms  = <EIO>        (unset — no autosuspend timer)
   runtime_suspended_time = 0
```

And there is **no `pm_runtime_put_autosuspend()` anywhere in the GPU's 3D path** —
every such call in the driver is in the *display* tree:

```
hdmi/hdmi_bridge.c, hdmi/hdmi_connector.c, mdp/mdp5/mdp5_{encoder,irq}.c
```

The only runtime-PM traffic on the GPU side is `pm_runtime_get_sync()` /
`pm_runtime_put_sync()` pairs, plus one in `adreno_get_param()`:

```
a6xx_gpu.c:3535
 *  1. adreno_get_param() wraps this in pm_runtime_get_sync/put_autosuspend.
```

So the "the GPU is suspended after every submit" mechanism from
`IMAGE-246-ZAP-LOADED.md` §5 does not exist on this tree today. The GPU stays
powered across submits.

**And the project had already been here**: `a6xx_gpu.c:3502-3504` records a
pm_runtime livelock that was found and fixed (images 203/204):

> *"This was the pm_runtime livelock: runtime_status pinned at … RCU stalls
> every ~63 s in pm_runtime_work, task state R. No panic, so no …"*

## 2. The freeze was the *display* device, not the GPU

The two power directories are different devices:

| path | device | state |
|---|---|---|
| `/sys/devices/platform/soc/2c00000.qcom,kgsl-3d0/power` | the **GPU** | `runtime_status=unsupported`, `control=auto` |
| `/sys/class/drm/card0/device/power` | **card0 = mdss_mdp, the display** | `runtime_status=**suspended**` |

`echo on > /sys/class/drm/card0/device/power/control` therefore forced the
**display** device out of runtime suspend — a full MDP/DRS power-up — and that is
what froze the device. The archived PMIC-reset captures show the same context:

```
Workqueue: pm pm_runtime_work
```

**Lesson: never touch `/sys/class/drm/card0/device/power/control` on this
device.** The GPU device's own power control is the separate
`.../2c00000.qcom,kgsl-3d0/power/control`, and it is not the same thing.

(The recipe's `echo on > .../2c00000.qcom,kgsl-3d0/power/control` targets the GPU
device and is a different, safer operation.)

## 3. Also fixed: the phone's root filesystem was 100 % full

```
before:  /dev/loop0  2.0G  2.0G  0  100% /
after:   /dev/loop0  2.0G  982M  964M  51% /
```

The project's old capture logs (`/root/cap-*.log`, `/root/c256.txt`, ~350 MB)
were moved to `/userdata/oldlogs`. This was not cosmetic: the backup copy of
`a630_sqe.fw` **failed with ENOSPC** because of it, and the ZAP blobs live in
`/lib/firmware/` — a full `/` can break firmware loading and sysfs writes.

## Status of the three open leads

| lead | status |
|---|---|
| ZAP secure firmware | **closed, negative** — loads, runs (`active_count(1)`), all three write paths still dead |
| autosuspend / runtime PM | **closed, negative** — the GPU is never runtime-suspended |
| a6xx trust configuration unprogrammed | **open**, but weaker than it was: the ZAP now loads and runs, so the missing piece is the *hookup*, not the firmware |
| EL2/HYP data abort (167/213) | **open** — still the dominant unexplained event, in the secure world |
