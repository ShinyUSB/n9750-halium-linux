# First BOOT flash — measured results

Device `RF8M82H3NFM`. Performed 2026-09-25 under explicit, single-shot
authorization for `heimdall flash --BOOT` of
`artifacts/flash/02-halium-boot-baseline-kernel.img` only.

Artifacts pulled from the device:

| File | What it is |
| --- | --- |
| `artifacts/device/logs/UefiLog1.txt` | Samsung bootloader UEFI log, from the `logfs` FAT volume |
| `artifacts/device/logs/logfs.img` | raw `logfs` partition image (8 MiB) |
| `artifacts/device/logs/last_kmsg` | `/proc/last_kmsg` (2 MiB) — **see the misattribution warning below** |
| `artifacts/device/logs/pmsg-ramoops-0` | `/sys/fs/pstore/pmsg-ramoops-0` (97 KiB) |

---

## Measured results against the required checklist

| Item | Result | Evidence |
| --- | --- | --- |
| BOOT flash completed | **YES** | `BOOT upload successful`, heimdall exit 0 |
| Image persisted in BOOT | **YES** | first 4096 B of `/dev/block/bootdevice/by-name/boot` hash to `46f8be527b33df17343d7d78dbcf160f59efa972622005ce57e76fca7cb6aa79`, identical to the same 4096 B of the flashed file; `ANDROID!` magic present; partition is 67,108,864 B, image is 52,633,600 B |
| Device identity confirmed | **YES** | `androidboot.em.model=SM-N9750`, `androidboot.bootloader=N9750ZSU6HZE1` |
| Storage confirmed UFS | **YES** | `androidboot.bootdevice=1d84000.ufshc`; `boot -> /dev/block/sda20`; UFS vendor SAMSUNG, product `KLUEG8UHDB-C2D1` |
| Bootloader accepted the image | **YES** | `AUTHENTICATE Succeed Kernel binary: boot`; `SEC CMDLINE : console=null androidboot.hardware=qcom ... firmware_class.path=/vendor/firmware_mnt/image` (our cmdline, read from our image); memory reserved at `0x8000000` (kernel) and `0x20000000` (ramdisk); `Shutting Down UEFI Boot Services` → handoff to ABL |
| Kernel output indicates successful boot | **UNKNOWN — no kernel log exists** | see below |
| userdata partition found | **UNKNOWN — no kernel log exists** | see below |
| `/data/rootfs.img` reported missing | **UNKNOWN — no kernel log exists** | see below |
| Panic handler ran | **UNKNOWN — no kernel log exists** | see below |
| USB RNDIS enumerated | **NO** | host never saw an `18d1:d001` device or a new network interface; `lsusb` only ever showed `04e8:685d` then `18d1:6860` (TWRP/recovery) |
| `192.168.2.15` responded | **NO** | the interface never appeared, so nothing was addressable |
| telnet port 23 reachable | **NO** | same reason |
| Phone remained running or rebooted | **REBOOTED (bootloop)** | user-observed; Android `init` in the prior-stock log shows `reboot, recovery`, and the device ended up in TWRP |

---

## The critical finding: the boot is undiagnosable by construction

**No kernel log from our Halium boot exists anywhere on the device.** Three
independent reasons, all confirmed:

1. **`console=null`** in the cmdline (inherited verbatim from the stock d2q
   cmdline). The kernel prints to no console at all.
2. **`CONFIG_PSTORE_RAM_CONSOLE` is not set** in the kernel we shipped. Verified:
   `CONFIG_PSTORE=y`, `CONFIG_PSTORE_RAM=y`, but
   `# CONFIG_PSTORE_RAM_CONSOLE is not set`. Without it the kernel never writes
   its printk stream to the ramoops area, which is exactly why
   `pmsg-ramoops-0` is 97 KiB of unreadable non-text: it is not a console dump.
3. **`CONFIG_LOG_BUF_SHIFT=17`** keeps a 128 KiB ring buffer, but only in RAM.
   It is lost on the reboot that ends the loop.

`netconsole` is not an option because there is no network before the rootfs is
mounted. There is no ramdump or crash partition on this device (`logfs` is the
only log-ish partition and it holds bootloader output). `/data/tombstones` and
`/data/anr` are empty. TWRP's own kernel also lacks
`CONFIG_PSTORE_RAM_CONSOLE`, so it cannot recover a console log either.

**Consequence:** the *reason* for the bootloop is not determinable from the
available evidence. It is not recorded. I am not going to guess at it.

---

## Misattribution warning — a trap worth documenting

`/proc/last_kmsg` (2 MiB) contains a complete `Linux version
4.14.190-ShinyKernel` boot at line 13940, which looks exactly like our kernel.
It is **not** our Halium boot. Evidence it is the phone's *prior* Android boot:

- `root=PARTUUID=97d7b011-54da-4835-b3c4-917ad6e73d74` — a stock Android
  `root=`, which our cmdline does not contain at all
- f2fs mount options `root=32768,resgid=5678,inlinecrypt` for `/data`
- `androidboot.verifiedbootstate`, `androidboot.keymaster`, `androidboot.ulcnt`
- full Android userspace: `init` (PID 1) reading `/vendor/build.prop`, SELinux
  policy enforcement, `servicemanager`, `system_server`, `logd`, wifi `dhd`,
  running for 8000+ seconds
- `Freeing initrd memory: 4008K` — the stock Android ramdisk, not our 4,106,247 B
  Halium initramfs
- `sec_log_store() 2018-01-23 05:03:05(TZ:00), reboot, recovery` — Android
  `init` rebooting into recovery, which is how the device ended up in TWRP

So the dev phone had ShinyKernel installed and was running stock Android before
this work. That log is a known-good Android boot and is useful as a *reference*,
but it says nothing about our Halium boot.

## A lead that is NOT a lead

`EDTBO check fail`, `Final Dtb version = 0`, `Dtbo_Version = 0`,
`Override DTB: GetBlkIOHandles failed loading user_dtbo!` all appear in the
bootloader log for **our** boot — but they also appear in the known-good Android
boot (`grep -c` = 2 in `last_kmsg`). This device has no `user_dtbo` partition,
so these messages are expected here and are **not** evidence of our failure.
Pinning the bootloop on them would have been wrong.

---

## What is established, and what is not

Established (RUNTIME-VERIFIED):

- The device is an SM-N9750 on SM8150 with UFS.
- Heimdall can flash it, and one Odin session per Download Mode entry is
  sufficient for a flash.
- The bootloader parses, authenticates and hands off our boot image without
  complaint, using our kernel cmdline and the correct load addresses.

Not established, because nothing was recorded:

- whether our kernel completed initialisation
- whether the DTB it received was correct
- whether the data partition was found
- whether the initramfs ran, panicked, or was never reached
- whether the panic handler's RNDIS path was even attempted

The next flash must be made observable before it is made successful. The
minimal change is `CONFIG_PSTORE_RAM_CONSOLE=y` in
`configs/d2q_halium_headless.config`, which preserves the kernel log to ramoops
where TWRP can read it over ADB with no display, no serial adapter and no
network. Adding `console=ttyMSM0,115200n8` to the cmdline costs nothing and
would additionally give live output if a UART is ever attached. Both require a
kernel rebuild and a new BOOT flash, neither of which is authorized yet.
