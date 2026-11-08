# SM-N9750 / d2q baseline artifact manifest

Generated: 2026-09-25

## Source

- Kernel tree: `/home/usb/ShinyKernel/Kernel`
- Kernel: Linux `4.14.190` (`Petit Gorille`), ARM64
- Defconfig: `arch/arm64/configs/d2q_chn_hk_defconfig`
- Variant: `CONFIG_SEC_D2Q_PROJECT=y`, `CONFIG_MACH_D2Q_CHNZH=y`
- SoC/device tree: Qualcomm SM8150, Samsung `davinci`
- Localversion: `-ShinyKernel`

## Build

The commands in `ShinyKernel/Kernel/build.txt` were used with its toolchain and
flags. The prescribed `-j8` invocation was attempted first and the compiler was
killed while compiling `net/bluetooth/hci_conn.c` due to host memory pressure.
The identical command was rerun with `-j2`; it completed successfully. No
compiler, linker, source, or Kconfig changes were made.

- Config SHA-256: `1d92ce65a4d6163d36ab0cd729f7a5ea0191e4c76d9dbe8d5e7e1497469f45e9`
- `Image` SHA-256: `0614e24478fee814c403372f5514b64b0aa52e3360729099eff0bd31dbbb8b31`
- `Image-dtb` SHA-256: `7dedfea614d179fa81b44ecd3e8c13d5285a7267a1846f6d0631f8a74af8e263`
- `vmlinux` SHA-256: `a8fce0de065a6cc8e794526d5bd7a6e4c768338ec10156bdd309b536f4693692`
- `System.map` SHA-256: `ed7dcdb5c26ea97964dfc4916cec2124133b2c465a5952e686ff2defb7401166`

## Output geometry

- `Image`: 47,521,808 bytes
- `Image-dtb`: 48,519,996 bytes
- `Image-dtb` begins with `UNCOMPRESSED_IMG` + little-endian image length
- Two base DTBs are built: `sm8150.dtb`, `sm8150-v2.dtb`
- Fifteen d2q DTBO files are built: CHN overlays r00-r04 and r06-r14, plus
  `sm8150-sec-system-update-overlay.dtbo`
- Kernel compression: uncompressed ARM64 `Image` with appended DTB blob

## Preserved files

- `shinykernel-baseline/Image`
- `shinykernel-baseline/Image-dtb`
- `shinykernel-baseline/vmlinux`
- `shinykernel-baseline/System.map`
- `shinykernel-baseline/config`
- `shinykernel-baseline/dts/`
- `shinykernel-baseline/build-j2.log`
- `stock-boot.img` and `stock-unpack/` (stock Samsung reference)
- `prebuild/` (prebuild config/logs and failed `-j8` log)

The stock boot image is retained only as a read-only reference. It is a
ramdisk-less Samsung boot image; it must not be overwritten or flashed as part
of this port.
