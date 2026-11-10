# d2q — Debian Linux on the Samsung Galaxy Note 10+ (SM-N9750, Snapdragon 855)

Work-in-progress research and bring-up notes for running a real Debian 12 (bookworm)
userspace on a "Chinese" Snapdragon Note 10+ (`d2q`, SM-N9750). The stock Samsung kernel lineage is patched to boot a Debian rootfs directly
(downstream 4.14.190 kernel, fbdev + DRM display path, kgsl GPU, ADSP audio, SLPI sensors,
modem stack via QRTR/QMI, all reverse-engineered from the stock vendor image).

This repository collects the *research*: how each subsystem was made to work, the dead ends,
the kernel/mesa patches, and the scripts used on the host and the device. It intentionally
contains **no vendor firmware or Samsung/Qualcomm binaries** (see NOTICE).

---

## Status (2026-11-08)

| Subsystem | State | Notes |
|---|---|---|
| Boot to Debian rootfs | ✅ | Samsung ABL → patched kernel → /data Debian rootfs (loop ext4) |
| Display (panel power/PLL) | ✅ | DSI 7nm PLL digital-top fix; DMS/rotation caveats documented |
| GPU — Vulkan (Turnip/kgsl fork) | ✅ | Minecraft/`vkcube` render at 25–30 fps |
| GPU — OpenGL (zink over kgsl Vulkan) | ✅ | Xorg glamor/zink, phoc/wlroots GL paths |
| Display — Xorg session | ✅ | Xorg :1 + openbox + lxpanel + pcmanfm (proven daily driver) |
| Display — Wayland session | ⚠️ | labwc 0.7.4 + wlroots 0.17.4 Vulkan renderer (GPU-composited, landscape); intermittent panel stalls under investigation, see `docs/audio/` & `docs/evidence/` |
| Audio — speakers | ✅ | cs35l41 ×2 amps on SEC_TDM_RX_0 via ADSP |
| Audio — microphones ×3 | ✅ | WCD934x DMICs (main/sub/3rd; "4th" is a mirror, proven by cross-correlation) |
| Audio — PipeWire | ✅ | sink+source, volume pins, end-to-end verified |
| Camera — 4 sensors + ToF depth | ✅ | CamX HAL shims; ToF point cloud via SecCamTOF trigger |
| NFC | ✅ | PN547 reader + card emulation |
| Sensors (SLPI/SEE) | ✅ | SSC over QRTR (libssc fork) |
| GNSS | ✅ | QMI LOC via gnss-share fork (NMEA) |
| Modem — voice/data | 🔶 | QMI stack up, IPA registration fixed; UL data path requires DSD layer (open) |
| Wi-Fi | ✅ | BCM4375 PCIe |
| Bluetooth | ✅ | BCM4375 UART (HCI attach scripts included) |

## Installation

See [INSTALL.md](INSTALL.md) — full path from stock firmware to a running desktop.

## Repository map

```
docs/
  display/     GPU/display bring-up corpus (355 notes: kgsl, GMU, a6xx, PLL, exercises)
  audio/       AUDIO-BRINGUP + MIC-CAPTURE + PIPEWIRE-PHOSH session docs
  camera-tof/  4K60 RCA, ToF depth working notes, property/teardown fixes
  telephony/   QMI/IPA/rmnet data-path investigation + DIAG/WDS reports
  nfc/         PN547 investigation
  sensors/     SLPI/SEE sensors, libssc notes
  evidence/    Live kernel evidence: crtc_commit stacks + labwc thread sampler logs
  screenshots/ Reference states of the working desktops
patches/
  kernel/      53-file d2q kernel delta (baseline → working tree): display, kgsl/a6xx port,
               IPA, service-locator, touch, watchdog, DTS/config; plus per-file diffs and
               the D2Q reference files. Includes the (deadlocking) atomic-commit kick experiments.
  mesa/        KGSL fork deltas vs upstream Mesa 25.3.0 (tu_knl_kgsl, zink screen, DRIL)
  misc/        ns-lite v2→v3 diff (QRTR name service)
scripts/
  host/        Build/flash/pack/ssh tooling (heist: pack-d2q.sh boot-image packer)
  device/      Session units + launchers (labwc/wayland, Xorg), audio/mic init, UDEV helpers
  recon/       Camera HAL shims, Ghidra scripts (47), netmgr/QMI-IPA C tools, QMI capture shims
configs/
  labwc/ waybar/ wofi/   Wayland desktop configuration (landscape 90° @2x, touch matrix)
  x11/         Weston-era units/ini kept for reference
  audio/       Stock mixer_paths_tavil.xml (capture routes) + WirePlumber volume pin
  kernel/      Kernel cmdline + clang wrapper used for the build
  systemd/     Device units (wlan, data-up)
```

## How it boots (short version)

1. Bootloader loads a packed BOOT image: stock 4.14.190 Samsung kernel + patched d2q deltas
   (`patches/kernel`), stock Android ramdisk (firmware staging), Samsung cmdline
   (`configs/kernel/cmdline-307v2.txt`).
2. The Debian rootfs lives on `/data` (loop-mounted ext4); the stock vendor partition stays
   mounted read-only and supplies firmware + audio mixer paths.
3. Systemd units bring up: ADSP via `boot_adsp/boot` + pd-mapper-lite + QRTR ns-lite,
   audio routes, mic routes, GPU clock pin, SLPI fastRPC, GNSS, WLAN/BT.
4. The desktop session is a normal systemd service (`d2q-xorg` or `d2q-labwc`).

Flash: `heimdall flash --BOOT <image>` (Download mode). Pack: `scripts/host/pack-d2q.sh`
(read its header — the boot-image layout traps are documented there).

## Research highlights

- **Display**: this CAF sde fork only presents cmd-mode frames when the DPU is "kicked";
  the DKMS-side `d2q_kick` workqueue (dirtyfb path) is why Xorg works. The atomic-commit kick
  variants for wlroots deadlock on `connection_mutex` — documented in `patches/kernel` and
  `docs/evidence/`.
- **GPU**: freedreno/msm on downstream kgsl needs kernel-side a6xx GMU/hfi support
  (ported in the delta) + a Mesa fork with kgsl winsys (`patches/mesa`); no GBM-on-msm
  pipe driver exists in that fork, so compositors must use the DRM dumb allocator.
- **Audio**: Samsung's `q6audio-adaptation` machine driver + WCD934x tavil DMICs; all routes
  recovered from the stock `mixer_paths_tavil_old.xml` and verified acoustically.
- **Telephony**: IPA v4.1 on SM8150 with the modem's Q6 WAN GSI channels gated behind the
  Android DSD layer — the open problem, with all 5 eliminated hypotheses documented.

## Legal / provenance

- All vendor firmware, Samsung/Qualcomm binaries, boot images and per-device secrets are
  **excluded** (see NOTICE). Docs reference them by name only.
- The kernel patches are GPL-2.0 (Samsung kernel lineage). Mesa deltas are MIT.
- Nothing here is affiliated with Samsung or Qualcomm.
