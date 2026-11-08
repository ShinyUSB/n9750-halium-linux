# The 284s reset — a DISPLAY vblank timeout, not the GPU

Kernel `#133`/`#134`. Evidence: `crashes/last_kmsg-1010-003522-7edf01cf.log`
(contains boot `899a4a8b`, image 192 — confirmed by `SMMU map itcmhole`).

## What happened

I expected a silent hard reset and was wrong: the archive holds a full named
backtrace. WLAN link-down is immediately followed by a display stall:

```
CFG80211-INFO) wl_handle_link_down : [wlan0] Disconnect event sent to upper layer
vblank wait timed out on crtc 0
------------[ cut here ]------------
WARNING: CPU: 0 PID: 500 at drivers/gpu/drm/drm_vblank.c:1072 drm_wait_one_vblank+0x248/0x258
CPU: 0 PID: 500 Comm: crtc_commit:133 Tainted: G S      W       4.14.190-ShinyKernel #133
```

```
Call trace:
  drm_wait_one_vblank+0x248/0x258
  _sde_encoder_update_rsc_client+0x52c/0x608
  sde_encoder_resource_control+0xcdc/0x1284
  sde_encoder_virt_enable+0x340/0x54c
  complete_commit+0x354/0xba4
  _msm_drm_commit_work_cb+0xcc/0x1a8
```

Every frame is CAF **display** code (`sde_encoder_virt_*`). Not one GPU frame.
And the reboot is deliberate:

```
UploadCause[KERNEL PANIC], Don't check hangcnt
collect_rr_data : upload_cause = KERNEL PANIC
```

That string is not in our source tree (CAF/TZ crash-reporter side), so the panic
originates below our driver — but the display timeout is the trigger.

## Root cause

**The reboot is the display subsystem, not the GPU.** A `crtc_commit` in
`sde_encoder_virt_enable()` waits for a vblank that never arrives, times out in
`drm_wait_one_vblank()` (`drm_vblank.c:1072`), and CAF's crash path takes the
system down.

Worth noting: `drm_vblank.c:1062-1071` gives Samsung builds a **1000 ms** timeout
via `CONFIG_DISPLAY_SAMSUNG` and everyone else **100 ms**. We are on the tighter
100 ms path.

## Why the GPU log showed nothing

Because the GPU really was fine. `gpu192.log` ends at 284 s with no SMMU fault,
no watchdog, no driver error, and the GPU had been serving accepted submits the
whole time. Two unrelated events in one boot.

## Ruled out

- **Not a watchdog.** No `/dev/watchdog`, no watchdog module, only
  `dhd_watchdog_thread` (WiFi). All four fuses off.
- **Not `softdog.soft_panic=1`** — that is a ramdisk cmdline default, not ours
  (our 19-word cmdline has only `watchdog_v2.enable=0`), and no softdog device
  exists on the running system.
- **Not a GPU fault** — zero SMMU faults on image 192.

## Honest gap

`WARN()` in `drm_vblank.c` is not fatal by itself, and
`# CONFIG_PANIC_ON_OOPS is not set`, so a WARN should print and continue.
Something in CAF's TZ/SSR layer escalates it to a reboot. **I found the trigger
but not the escalation mechanism**, because that string is not in our tree. I am
not going to guess at it.

## Disposition

This is a display-path bug, off the path to rendering. It costs a boot every
~5 min, which is tolerable while testing the GPU because
`harness/gpu-capture.sh` writes to disk on the phone and survives it.