# Bring-up staging (BUILD-VERIFIED, NOT RUNTIME-VERIFIED)

No device has been connected. Nothing here proves the phone boots.
Flash in this order: test 02 first to isolate kernel+boot chain from userspace,
then 01.

## 01 — primary boot image (Halium-profile kernel)

    heimdall flash --BOOT 01-halium-boot-halium-kernel.img

## 02 — baseline-kernel boot image (same initramfs, known-good kernel)

    heimdall flash --BOOT 02-halium-boot-baseline-kernel.img

Use 02 first. If 02 enumerates USB and 01 does not, the fault is in the kernel
config fragment, not in the Halium userspace. If neither enumerates, the fault is
in the boot chain or the initramfs.

## 03 — rootfs image (NOT written by Heimdall)

This must be placed on the userdata partition as `/data/rootfs.img`, because
`/scripts/halium` in the initramfs looks for exactly that path:

    identify_file_layout() {
        if [ -e /tmpmnt/rootfs.img ]; then   # <- /data/rootfs.img
            imagefile=/tmpmnt/rootfs.img
            file_layout="halium"

Heimdall only writes whole partitions it knows from the on-device partition table,
so it cannot place a file inside userdata. Use ODIN in Download Mode with
HOME_CSC and the CP/USERDATA slot set to `/data/rootfs.img`, or push it with
fastboot/adb from a previously working system.

This step replaces the contents of userdata. It is the only destructive step in
the procedure. Nothing in `tools/` performs it.

## Recovery — always available

    heimdall flash --BOOT /home/usb/ShinyKernelREVIEWED/Kernel/stock/boot.img

No PIT, no repartitioning, no userdata erase, no bootloader unlock.

## 2026-10-29 — image 301 (features bring-up)

`301-halium-boot-penres-rmtfs.img` (sha256 fc945b7e9867789611bd13db5f9e174ac0805085267c7404ae3768887f3ffbc1,
kernel Image-dtb 2f4534c384eb5a74…) = image 300 kernel + (1) Wacom W9020 axis resolution
(drivers/input/wacom/wacom_i2c.c input_abs_set_res after the xy_switch abs-params block) and
(2) CONFIG_UIO_MSM_SHAREDMEM=y (drivers/uio/msm_sharedmem → /dev/qcom_rmtfs_uio0 for the rmtfs
daemon). Ramdisk = canonical eb5d2eb5…, cmdline = 300's 454-byte cmdline; REFERENCE header-diff vs
300 passed (id-field only). Purpose: libinput-native S Pen tablet + modem RMTFS (GPS). Session doc:
artifacts/gpu-bringup/STATE-FEATURES-BRINGUP-2026-10-29.md.
