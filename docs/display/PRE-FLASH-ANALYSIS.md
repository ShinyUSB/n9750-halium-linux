# Pre-flash analysis: boot image, initramfs trace, and the `/data/rootfs.img` path

Prepared 2026-09-25, before any flash. Nothing in this document was obtained by
flashing; every statement is from reading the actual images and the device's own
PIT, or from executing the image contents on the host.

---

## 1. `02-halium-boot-baseline-kernel.img` — verified

| Field | Value | Verdict |
| --- | --- | --- |
| Path | `/home/usb/artifacts/flash/02-halium-boot-baseline-kernel.img` | |
| Size | 52,633,600 bytes | |
| SHA-256 | `63d5a180a9615afa70e10a1411b3a86c02ea16f14c18b444273172321be600b9` | |
| `boot_magic` | `ANDROID!` | ok |
| header version | 1 | matches stock |
| page size | 4096 | matches stock |
| kernel load addr | `0x00008000` | matches stock |
| ramdisk load addr | `0x02000000` | matches stock |
| second load addr | `0x00f00000` | matches stock |
| tags load addr | `0x01e00000` | matches stock |
| board / product name | `FRPSC27A006` | matches the CHN/HK stock image |
| second_size | 0 | none expected |
| os_version | 0 | n/a |

Header field offsets were read directly from the file (magic@0, kernel_size@8,
kernel_addr@12, ramdisk_size@16, ramdisk_addr@20, second_addr@28, tags_addr@32,
page_size@36, header_version@40, name@48, cmdline@64) rather than trusted from
the unpack tool.

### Kernel inside the image

- **SHA-256 `7dedfea614d179fa81b44ecd3e8c13d5285a7267a1846f6d0631f8a74af8e263`**
  — byte-identical to the preserved known-good baseline
  `artifacts/known-good/shinykernel-baseline/Image-dtb`, and **not** the
  Halium-profile kernel (`d05f97b1...`). This is exactly the intent: image 02 is
  the baseline-kernel control.
- Version string: `Linux version 4.14.190-ShinyKernel (usb@shinymachine)
  (clang version 10.0.9, GNU ld 2.27.0.20170315) #1 SMP PREEMPT Fri Sep 25 17:54:21 +03 2026`
- ARM64: the first 16 bytes are the Samsung `UNCOMPRESSED_IMG` tag, so `file`
  reports "data". The first real instruction word at offset 16 is `0x02d52010`,
  a valid AArch64 encoding, confirming an aarch64 kernel.
- That 4-byte value at offset 16 is the kernel load address, emitted by the
  kernel build as `printf \020\050\325\002` into `Image-dtb-hdr`.
- **Two base DTBs are appended**, located by scanning for the FDT magic:
  - offset 47,521,828 — `Qualcomm Technologies, Inc SM8150 v2 SoC`, `qcom,sm8150`, model `SM8150 V2`
  - offset 48,022,051 — `Qualcomm Technologies, Inc SM8150 v1 SoC`, `qcom,sm8150`, model `SM8150 V1`

  Both SoC revisions are present, so the bootloader can select the right one.
  The 15 `sm8150-sec-d2q-chn-overlay-r*.dtbo` overlays are **not** in the boot
  image; they live in the `dtbo` partition, which this flash does not touch, so
  the stock overlays remain in effect.

### Ramdisk inside the image

- SHA-256 `0bbac4577f3567aec935c958216de5d30c7355452ca56248d6728f4f2634bdb6`
  — the verified upstream Halium ARM64 initramfs, 4,106,247 bytes, gzip.

### Kernel command line

```
console=null androidboot.hardware=qcom androidboot.console=ttyMSM0
androidboot.memcg=1 lpm_levels.sleep_disabled=1
video=vfb:640x400,bpp=32,memsize=3072000 msm_rtb.filter=0x237
service_locator.enable=1 swiotlb=2048
androidboot.usbcontroller=a600000.dwc3
firmware_class.path=/vendor/firmware_mnt/image loop.max_part=7
```

This is the **stock d2q command line**, unmodified. Notably absent:

| Token | State | Consequence |
| --- | --- | --- |
| `root=` | absent | the generic initramfs `ROOT` stays empty; irrelevant because `conf/conf.d/halium` sets `BOOT=halium` and the Halium `mountroot()` locates the rootfs itself |
| `rootfstype=` | absent | not used on the Halium path |
| `datapart=` | absent | **the data-partition override is not set**, so discovery relies entirely on the name search in section 3 |
| `systempart=` | absent | not used |
| `androidboot.slot_suffix=` | absent | no A/B handling, consistent with this device |
| `break=` | absent | no initramfs breakpoint |
| `panic=` | absent | **important**: `panic()` therefore does *not* auto-reboot; it runs `/scripts/panic` and blocks |

### Target confirmation

- Platform from the device's own PIT: `CPU/bootloader tag: SM8150`.
- Platform from USB enumeration: `Product: SM8150`, `Manufacturer: Samsung`.
- DTBs in the image: `qcom,sm8150` V1 and V2.
- `ShinyKernel` is the known-good tree for this device per the task, and the
  kernel in this image is byte-identical to its reproduced baseline.

Conclusion: this image is built for SM-N9750/d2q on SM8150, uses the stock boot
geometry, and targets the BOOT partition only.

---

## 2. The initramfs, traced step by step

`initrd.img-touch-arm64` extracted; relevant files are `/init`,
`/scripts/functions`, `/scripts/halium`, `/scripts/panic/telnet`,
`/conf/conf.d/halium`.

### 2a. Immediately after the kernel starts

`/init` runs:

1. sets a fixed `PATH`, creates `/dev /root /sys /proc /tmp /var/lock`
2. mounts `sysfs` on `/sys`, `proc` on `/proc`
3. reads `/proc/cmdline`; `console=null` means it prints "Loading, please wait..."
4. mounts **devtmpfs on `/dev`** — this is why device nodes appear without udev
5. mounts `devpts` on `/dev/pts` and a tmpfs on `/run`
6. sources `/conf/arch.conf` and `/conf/initramfs.conf`, then
   `/conf/conf.d/*` → **`conf/conf.d/halium` sets `BOOT=halium` and
   `MODULES=list`**
7. sources `/scripts/functions`, parses the command line
8. `run_scripts /scripts/init-top` (empty in this image)
9. `maybe_break modules`; **`load_modules()`**
10. `run_scripts /scripts/init-premount` (absent)
11. `maybe_break mount`; sources `/scripts/local`, `/scripts/nfs`, and
    `/scripts/${BOOT}` → **`/scripts/halium`**, then `mount_top`,
    `mount_premount`, **`mountroot()`**
12. `mount_bottom`, `local_bottom`, `run_scripts /scripts/init-bottom`
13. `mount -n -o move /run ${rootmnt}/run`
14. `exec run-init ${rootmnt} /sbin/init`

### 2b. **The initramfs contains no kernel modules**

`find . -name '*.ko'` returns **0**. `/conf/modules` is empty/absent, so
`load_modules()` loads nothing. Every driver the boot path needs must therefore
be **built into the kernel**. Verified in the config actually shipped:

| Symbol | Value | Needed for |
| --- | --- | --- |
| `CONFIG_USB_DWC3` | `y` | the UDC |
| `CONFIG_USB_DWC3_DUAL_ROLE` | `y` | OTG/role switching |
| `CONFIG_USB_DWC3_OF_SIMPLE` | `y` | DT glue |
| `CONFIG_USB_DWC3_MSM` | `y` | **the Qualcomm/SM8150 variant** |
| `CONFIG_USB_GADGET` | `y` | gadget stack |
| `CONFIG_USB_CONFIGFS` | `y` | configfs gadget API |
| `CONFIG_USB_CONFIGFS_RNDIS` | `y` | RNDIS function |
| `CONFIG_USB_F_RNDIS` | `y` | RNDIS function |
| `CONFIG_EXT4_FS` | `y` | mount `rootfs.img` |
| `CONFIG_BLK_DEV_LOOP` | `y` | `mount -o loop` the rootfs image |
| `CONFIG_DEVTMPFS` / `_MOUNT` | `y` | device nodes |

No DWC3/UDC/ULPI symbol is `=m`. `CONFIG_USB_ETH` and `CONFIG_USB_ETH_RNDIS`
are **not set**; that is the legacy Android USB-ether path and is not used —
both the panic script and our `d2q-rndis.sh` use **configfs** RNDIS. This is
consistent, but worth stating because `PORTING.txt` lists those two symbols.

`vmlinux` string scan confirms the built-in symbols: `dwc3`, `configfs`,
`rndis`, and `android_usb`.

### 2c. How the data partition is discovered

Inside `mountroot()`:

```sh
partlist="userdata UDA DATAFS USERDATA"

udevadm settle

for partname in $partlist; do
    part=$(find /dev -name $partname | tail -1)
    [ -z "$part" ] && continue
    path=$(readlink -f $part)
    [ -n "$path" ] && break
done

# A/B slot suffix, if androidboot.slot_suffix= is present (it is not)
...
# explicit override, if datapart= is present (it is not)
if grep -q datapart= /proc/cmdline; then ... path=${x#*=} ... fi

if [ -z "$path" ]; then
    halium_panic "Couldn't find data partition."
fi
```

**Exact nodes searched:** `find /dev -name <name>` with `name` in
`userdata`, `UDA`, `DATAFS`, `USERDATA`, taking the **last** match and
`readlink -f`-ing it. Because `find -name` matches the *basename* anywhere under
`/dev`, this will match:

- `/dev/disk/by-partlabel/userdata` → resolves to the real block device, and also
- a device node literally named `userdata`

`udevadm settle` is called first, so the `by-partlabel` symlinks should exist by
then. This is the reason the UFS-vs-eMMC question is *probably* fine: it does not
depend on `/dev/sdaN` naming, it depends on GPT partition labels being exposed.
`udevadm settle` failing does not abort the script (no `set -e`).

Then: `e2fsck -y $path`, `resize_userdata_if_needed`, and
`mount -o discard,data=journal $path /tmpmnt`.

### 2d. When `/data/rootfs.img` is required, and what happens if absent

`identify_file_layout()`, called from `mountroot()` after userdata is mounted on
`/tmpmnt`:

```sh
if   [ -e /tmpmnt/rootfs.img ];      then imagefile=/tmpmnt/rootfs.img; file_layout="halium"
elif [ -e /tmpmnt/ubuntu.img ];      then imagefile=/tmpmnt/ubuntu.img;  file_layout="legacy"
elif [ -d /tmpmnt/halium-rootfs ];   then imagefile=/tmpmnt/halium-rootfs; file_layout="subdir"
else                                       file_layout="partition"
fi
```

So `/data/rootfs.img` is required **before the switch into the rootfs** — at
`mountroot()` time, i.e. very early, long before any userspace of ours runs.

If it is **absent**, `file_layout=partition` and `$imagefile` is empty. The
script then:

- skips the `mount -o loop,rw $imagefile` (all three branches test false on an
  empty string)
- logs `WARNING: Android system image not found.`
- `mount -o remount,ro /halium-system` fails harmlessly (not a mountpoint)
- logs `WARNING: Failed to mount Android system.img.`
- `identify_boot_mode` → `BOOT_MODE='halium'`
- both the `android` branch and the `Normal boot` branch are skipped, because
  both test `[ -e $imagefile ]` against an empty string
- reaches:

```sh
else
    # Possibly a re-partitioned device
    halium_panic "Couldn't find a system partition."
fi
```

### 2e. It panics, but it does **not** die — and it exposes USB first

This is the most important result of the trace. `panic()` in `/scripts/functions`:

```sh
panic() {
    ...
    if [ -n "${panic}" ]; then        # our cmdline has NO panic= ...
        sleep ${panic}; reboot; exit
    fi

    run_scripts /scripts/panic       # <-- THIS runs
    ... open an interactive console shell ...
}
```

`/scripts/panic/` contains exactly one handler: `telnet` (plus its `ORDER`
file). `scripts/panic/telnet` then:

1. `usb_setup` — mounts configfs on `/config` and creates a gadget
   `18d1:d001`, serial `halium-initrd telnet 192.168.2.15`,
   manufacturer `Halium initrd`, **product string `Failed to boot`**.
   It prefers `/sys/class/android_usb/android0` if present, else configfs.
2. `ifconfig rndis0 192.168.2.15`, falling back to `ifconfig usb0 192.168.2.15`
3. if neither interface appears: `dmesg`, `sleep 60`, **`reboot -f`**
4. otherwise starts **`udhcpd`** handing out `192.168.2.20`–`192.168.2.90`
   from `/etc/udhcpd.conf`
5. starts **`telnetd -b 192.168.2.15:23 -l /bin/sh`** — a root shell
6. dumps `ps`, `netstat -lnp`, `/proc/mounts` to the console
7. enters `inject_loop`, which **blocks** on a FIFO at `/init-ctl/stdin`
   (commands typed there are executed; `continue` exits the loop)

Because the loop blocks and no `panic=` is set, **the device stays alive in a
diagnostic state indefinitely**, serving RNDIS and telnet. `/tmpmnt` — the
mounted userdata — is still present and writable in that shell.

**Therefore: a missing `/data/rootfs.img` does not prevent diagnostics. It
produces exactly the debug session the staged test plan needs.**

### 2f. Two concrete risks in that path

1. **`rndis_bam` is not built.** The panic script unconditionally does
   `mkdir $GADGET_DIR/g1/functions/rndis_bam.rndis` and then symlinks it into
   the config. The shipped kernel has `rndis` and `dwc3` in `vmlinux` but **no
   `rndis_bam`** string, and there is no `CONFIG_USB_F_RNDIS_BAM`. So the mkdir
   will fail and leave a **dangling symlink inside the USB config**, which can
   make the `UDC` bind fail. The script has no `set -e`, so it will not abort —
   it will just fail to bring the gadget up, and then hit the `reboot -f` path.
   *Mitigation if observed:* rebuild the initramfs with those two lines removed,
   or rely on the `android_usb` path if `/sys/class/android_usb/android0` exists
   (`android_usb` **is** present in `vmlinux`).
2. **Static IP only on the device side.** `udhcpd` is started, but the host here
   has no DHCP client (`dhclient`, `dhcpcd`, `udhcpc` all absent). Using a
   static `192.168.2.100/24` on the host avoids needing one.

---

## 3. Getting `rootfs.img` onto `/data` — a path that needs no Android

The image to place: `artifacts/flash/03-rootfs-arm64.img`,
2,147,483,648 bytes, SHA-256
`85261074060188830b3bcc1def0e5ca500ae39a13b1a77b98854834d946be7d8`.

### Why the standard routes do not apply

- **Heimdall** writes whole partitions it learns from the on-device table. It
  cannot place a file *inside* userdata.
- **Odin4** (`7.3.0-a46321b`, native Linux) takes only firmware archives:
  `-b` bootloader, `-a` AP, `-c` CP, `-s` CSC, `-u` UMS, all `.tar`/`.tar.md5`.
  There is no raw-image slot and no USERDATA-file slot. It is a stock-firmware
  flasher, not a custom-image flasher.
- **halium-install** would have done it, but its non-test path is
  `flash_dir` / `flash_img`, which **push to `/data` via ADB** — that needs a
  working Android system first, which is the thing we do not have. Circular.

### The workable route: use the initramfs we already ship

The initramfs contains an **aarch64 statically linked busybox** (1,553,912 bytes)
with the applets `wget`, `nc`, `dd`, `sha256sum`, `httpd`, `tftp`, `telnetd`,
`udhcpd`, `udhcpc`, `ifconfig`, `route`, `df`.

So the device can **pull** the image over the very RNDIS link it just created:

```sh
# on the phone, in the panic telnet shell (or via /init-ctl/stdin)
wget http://192.168.2.100:8000/rootfs-arm64.img -O /tmpmnt/rootfs.img
sha256sum /tmpmnt/rootfs.img     # must equal 8526107406...
sync
```

Host side:

```bash
ip addr add 192.168.2.100/24 dev <detected-rndis-iface>
ip link set dev <detected-rndis-iface> up
cd /home/usb/artifacts/flash && python3 -m http.server 8000
telnet 192.168.2.15 23            # or drive /init-ctl/stdin
```

Properties of this route:

- **No repartitioning, no PIT, no userdata wipe, no EFS, no CSC.** It writes one
  file, `/data/rootfs.img`, exactly where the initramfs looks for it.
- **No Android involved at any point.**
- **Checksum verified on the device** before the next boot.
- `wget` runs over USB 2.0 high-speed RNDIS (the device enumerates at 480 Mbit/s),
  so 2 GB is minutes, not hours.
- After it lands, a normal reboot makes `identify_file_layout` find
  `file_layout="halium"` and proceed into the rootfs.

### Userdata encryption — does it block this?

`/data` on d2q uses Android FBE (f2fs/ext4 with per-directory fscrypt keys
derived from a hardware keystore). That does not block this route:

- FBE encrypts *specific namespaces*; it is not whole-partition encryption.
- The initramfs already writes into userdata itself — `mount_android_partitions`
  runs `mkdir ${real_userdata}/cache` — so the filesystem is writable from
  outside Android.
- The file we write is a plain, unencrypted file. The initramfs loop-mounts it
  directly, so it never goes through Android's key management. Android would
  simply see an extra file at the `/data` root; it is not encrypted, so it is
  not a secret, and stock Android is unaffected in any way that matters.
- This is not a new partitioning scheme and introduces no new on-disk format.

Residual unknown, to be settled by measurement: whether d2q userdata is f2fs or
ext4, and the partition label the initramfs's `find` will match. The panic
telnet session settles both in one shot (`blkid /tmpmnt`, `ls -l /dev/disk/by-partlabel/`).

---

## 4. Flash authorization block (not executed)

Per instruction, no flash has been performed and none will be until explicitly
authorized.

```
TARGET:      SM-N9750 / d2q  (PIT tag SM8150, USB product string SM8150)
PARTITION:   BOOT only
IMAGE:       /home/usb/artifacts/flash/02-halium-boot-baseline-kernel.img
IMAGE SIZE:  52633600 bytes
IMAGE SHA256:63d5a180a9615afa70e10a1411b3a86c02ea16f14c18b444273172321be600b9
KERNEL:      4.14.190-ShinyKernel baseline Image-dtb
             sha256 7dedfea614d179fa81b44ecd3e8c13d5285a7267a1846f6d0631f8a74af8e263
             (byte-identical to artifacts/known-good/shinykernel-baseline/Image-dtb)
             appended DTBs: qcom,sm8150 V2 @47521828, qcom,sm8150 V1 @48022051
INITRAMFS:   Halium ARM64, 4106247 bytes
             sha256 0bbac4577f3567aec935c958216de5d30c7355452ca56248d6728f4f2634bdb6
             0 kernel modules; all required drivers built into the kernel
PIT ENTRY:   BOOT is a standard AP entry; Heimdall resolves it from the device's
             own on-device partition table. No PIT is supplied or written.
             The on-device PIT (read-only, for inspection) has 76 entries,
             tag SM8150, Device Type 8 (UFS).
DATA SAFETY: no PIT written / no repartition / no EFS / no userdata wipe
             no CSC / no bootloader write / no stock firmware
RECOVERY:    heimdall flash --BOOT /home/usb/ShinyKernelREVIEWED/Kernel/stock/boot.img
             sha256 6718aba700e9850f76117f9f2ee74f56de7bbcd12eb324b7443a6cfb5bcae209
SESSION COST: exactly one Odin session is available per Download Mode entry.
             The flash should therefore be the FIRST session-consuming command.
```

---

## 5. What is still unproven

- Whether this vendor 4.14.190 kernel boots the Halium initramfs on d2q.
- Whether the data partition is discovered by the `find /dev -name userdata`
  search on this UFS layout, and under which partlabel.
- Whether the panic script's configfs path works given the missing `rndis_bam`,
  or whether the `android_usb` path is taken instead.
- Whether the kernel's initramfs handoff and `run-init` into systemd succeed.
- Nothing at all about RNDIS, ICMP or SSH on the device.

None of these are claimed. The point of the first flash is to measure them, and
the panic-telnet path exists precisely so the first flash yields information even
when the rootfs is missing.
