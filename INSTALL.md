# INSTALL — from a stock SM-N9750 to a running Debian desktop

This is the honest, complete path. **You cannot flash our boot images**: they contain your
device's stock Android ramdisk and Samsung firmware staging — material we neither own nor
ship (see NOTICE). You will build two things yourself: a Debian rootfs image and a patched
kernel boot image. Both steps are scripted.

Target hardware: **SM-N9750 (d2q, Snapdragon 855/SM8150, "Chinese" variant)**. The Exynos
N975F is a completely different device; none of this applies.

---

## 0. Prerequisites

On the host (Linux):
- `heimdall` (Download-mode flashing), `adb` (TWRP interaction), `podman` (rootfs base image),
  `mke2fs` (e2fsprogs), `python3`.
- ~30 GB free disk (kernel build tree + rootfs staging + images).

On the phone:
- SM-N9750, bootloader **OEM-unlocked** (Settings → Developer options → OEM unlock; this
  wipes data once), and a TWRP recovery image for this model available on your PC.
- One full charge, and the stock firmware of your exact model/build on the PC
  (you need its `BOOT`/`vendor` context; e.g. from Sammobile — `N9750ZSU…`).

## 1. Build the Debian rootfs (host, no root needed)

The rootfs is assembled from the official `debian:bookworm-slim` arm64 base image plus a
resolved Debian package closure — no qemu, no root, no maintainer scripts:

```bash
cd d2q-public
./scripts/host/build-rootfs-arm64.sh        # -> artifacts/generated/halium-rootfs-arm64
./scripts/host/prepare-rootfs.sh            # ssh keys, RNDIS unit, headless defaults
./scripts/host/pack-rootfs.sh               # -> artifacts/generated/rootfs.img  (ext4, 2 GB)
```

Result: `rootfs.img` — a plain ext4 image that the boot ramdisk mounts as the root filesystem
from `/data`.

## 2. Build the patched kernel boot image (host)

The kernel is Samsung's SM8150 4.14.190 lineage, patched with `patches/kernel/*.patch`
(display/GPU/IPA/touch/DTS deltas). Build recipe (containerised):

1. Obtain the SM8150 Samsung kernel source for your variant
   (opensource.samsung.com — package for the N9750 build you run).
2. Apply the patch set on top:
   ```bash
   cd Kernel
   patch -p1 < ../d2q-public/patches/kernel/01-display-d2q.patch
   patch -p1 < ../d2q-public/patches/kernel/03-other.patch
   ```
   (Per-file diffs are in `patches/kernel/per-file/` if you prefer to cherry-pick.)
3. Build in an Ubuntu 18.04 container with the Snapdragon LLVM clang 10.0.9 toolchain —
   `scripts/host/build-halium-kernel.sh` is the reference implementation (it automates the
   `REAL_CC`, `CROSS_COMPILE`, defconfig and DTBO steps; override `KERNEL_DIR`,
   `BASELINE_CONFIG`, `FRAGMENT`, `KERNEL_OUT` for your paths). Config fragment: ad
   `configs/kernel/`+ `d2q_chn_hk_defconfig` from the patch set.
4. Pack the boot image with `scripts/host/pack-d2q.sh`:
   - `KERNEL` = your built `Image-dtb`
   - `RAMDISK` = **the ramdisk extracted from your own stock boot image** (the packer's
     `--help`/header documents the extraction layout: kernel_size at offset 8,
     ramdisk_size at 16, page at 36)
   - `CMDLINE_FILE` = the cmdline of your stock boot image (compare with
     `configs/kernel/cmdline-307v2.txt`; the required flags: `msm_drm.fbdev=Y`,
     `drm_kms_helper.fbdev_emulation=Y`, `fbcon=map:2 vt.global_cursor_default=0`,
     `msm_drm.dsi_display0=…`, `loop.max_part=7`)
   - `REFERENCES`/`REFERENCE` = your stock boot image for the header-diff check
   The script refuses to pass if the layout differs beyond the kernel/id fields.

## 3. Flash (order matters)

```bash
# a) verification-disabled vbmeta (required for any custom BOOT; flags byte 0x2)
avbtool make_vbmeta_image --flags 0x2 --output vbmeta-debian.img
heimdall flash --VBMETA vbmeta-debian.img

# b) your packed boot image (phone in Download mode: heimdall detect first)
heimdall flash --BOOT d2q-boot.img
```

c) Install the rootfs image into `/data`:

```bash
# boot TWRP (vol keys), then over adb:
adb push rootfs.img /data/rootfs.img
```

The stock vendor partition is left untouched — the Debian system mounts it read-only and
uses the vendor firmware and the stock audio mixer paths from it.

## 4. First boot

The boot ramdisk mounts `/data/rootfs.img` and switches root. Networking is RNDIS:
host `10.15.19.1/24`, device `10.15.19.82/24`.

```bash
ssh root@10.15.19.82        # key: artifacts/keys/d2q_rsa from step 1
```

Then install the device services this repo ships (all systemd units/scripts are in
`scripts/device/` and `configs/`):

- `d2q-adsp.service` + pd-mapper-lite + qrtr-ns-lite — ADSP/audio bring-up (PDR chain)
- `d2q-mic.service` — microphone routing (all three mics)
- `d2q-audio-init` — cs35l41 speaker path
- `d2q-xorg.service` — X11 desktop (openbox + lxpanel + pcmanfm), portrait
- `d2q-labwc.service` — Wayland desktop (labwc 0.7 + wlroots 0.17 Vulkan renderer), landscape
- `d2q-wlan.service`, BT attach units — connectivity

## 5. Known caveats

- **Landscape on X11 is not possible** — this panel's DRM plane has no rotator (rotation
  blanks the panel). Landscape only via the wlroots session (software transform).
- **Wayland panel stalls** — under investigation; reproducible when USB devices are
  attached. See `docs/evidence/` for the captured thread stacks.
- Kernel builds are heavy (LLVM 10.0.9 + Ubuntu 18.04 container) and the workdir must not be
  on a full filesystem; see the trap notes in `scripts/host/build-gpu.sh`.