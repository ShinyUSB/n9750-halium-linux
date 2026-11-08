# Mesa render attempt — first run against a LOADED GPU (image 183)

## Setup

- USB RNDIS channel re-established (see below), so the trigger is observed
  live instead of costing a reboot. Host `enp0s20f0u3` = 10.15.19.100/24,
  phone `rndis0` = 10.15.19.82/24, 0.8ms rtt.
- Phone cool (35-38C), battery only, no WiFi dependency for the run.
- Trigger fired detached via `render183.sh`.

## Result — bring-up WON first ticket, render never got to draw

```
=== render183 start uptime=485s ===
WON on ticket 1 at uptime=486s
[485.925228] SELFTEST142: done last_iter=10 ALL 10 PASS
[485.925238] HWINIT[25] return ret=0
[485.925301] POSTINIT[40] RETURN_TO_CALLER ret=0
--- thermals at render --- 36600 40900 44000 ... (36-44C, fine)
--- render ---
<hung>
```

Bring-up is clean and repeatable: lottery won on attempt 0, no `-110`,
no retry, no wedge, phone stayed responsive on USB the whole time. The
async trigger (182) + TCM-aperture gate (183) + slumber-off (182) all
held.

## The blocker: MSM_PARAM_TIMESTAMP wedges in an OOB handshake

`egl-proof.py` never printed a single line — not even `DEVICES:`. Its
first DRM ioctl is `MSM_PARAM_TIMESTAMP` (Mesa queries the timestamp on
context create). `/proc/1399/stack` of the stuck python3:

```
__switch_to+0x10c
rpm_resume+0x280
__pm_runtime_resume+0x28
adreno_get_param+0x110
msm_ioctl_get_param+0x3c
drm_ioctl_kernel+0x110
do_vfs_ioctl+0x6bc
SyS_ioctl+0x90
```

State `D`, >2min, wchan `rpm_resume`. Walking the source:

- `adreno_get_param()` case `MSM_PARAM_TIMESTAMP`
  (`adreno_gpu.c:74-84`) does `pm_runtime_get_sync(&gpu->pdev->dev)`.
- `a6xx_get_timestamp()` (`a6xx_gpu.c:1372`) calls
  `a6xx_gmu_set_oob(gmu, GMU_OOB_GPU_SET)` — a live OOB handshake with
  the GMU firmware — then reads `RBBM_PERFCTR_CP_0`, then clears OOB.

On this run's firmware the OOB handshake never completes, so the
`pm_runtime_get_sync` never returns and the process sits in D forever.
The GPU is otherwise fine: all ten CP self-tests passed moments
earlier, and there is not a single GPU/HFI/GMU IRQ count in
`/proc/interrupts` (the `487/488/489` lines are absent entirely on this
boot — worth a separate look, but not the blocker here).

So: Mesa never got as far as creating a surface. This is a *real*
driver bug on the bring-up path, not a lottery failure and not a
regression from 181-183.

## Candidate fixes (image 184), in order of least risk

1. **Don't take runtime PM for the timestamp.** The timestamp read is a
   handful of MMIO reads with the GPU already powered (bring-up left it
   on; slumber is off since 182). Drop the
   `pm_runtime_get_sync`/`put_autosuspend` pair for
   `MSM_PARAM_TIMESTAMP` and read directly, or gate it behind the same
   "is the GPU actually up" state the rest of the driver checks. Mesa
   will accept a timestamp read that skips PM; it needs the value, not
   the PM dance.
2. **Bound the OOB set in `a6xx_get_timestamp`.** Even with (1), the OOB
   handshake must have a timeout and a non-fatal fallback. If GMU is
   silent, return the last known timestamp (or 0) instead of wedging a
   D-state task forever. Same class of fix as the OOB timeouts already
   proven survivable (`-110` paths).
3. Verify `MSM_PARAM_GPU_ID` (which needs no PM) answers, so we can
   confirm Mesa reaches the driver at all — a 2-minute sanity check once
   (1) lands.

## Also fixed this session: the USB RNDIS channel

Root cause of my earlier configfs `EPERM`s: this vendor kernel nests
gadget functions INSIDE the gadget dir
(`usb_gadget/<gadget>/functions/rndis.usb0`), not at
`usb_gadget/functions`. The known-good `tools/d2q-rndis.sh` had it
right all along and I did not read it first. RNDIS is built into the
kernel (`CONFIG_USB_RNDIS=y`, `CONFIG_USB_F_RNDIS=y`) — no reflash
needed. Host-side module is `rndis_host` (not `rndis`); it binds the
device as `enp0s20f0u3`. Note `d2q-rndis.service` is gone from the
device (only `d2q-wlan.service` remains) — that removal is why we lost
telemetry. `d2q-rndis` is staged at `/usr/local/sbin/d2q-rndis.sh` and
should be re-enabled as a service so the channel survives reboots.

State to be aware of: phone has a hung python3 (pid 1399) in D holding a
runtime-PM ref. That is survivable (no reset — 183 holds) but the GPU
will refuse further resumes until it clears. A reboot clears it.