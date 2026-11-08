# Iteration 2: measured results — flash completed, and a real kernel log obtained

Authorized: one BOOT rebuild adding three ramoops cmdline params, one BOOT
flash. Both done. No other partition touched, no recovery flash.

## 1. Pre-flash report (as required)

| | |
| --- | --- |
| old image (was on device) | `63d5a180a9615afa70e10a1411b3a86c02ea16f14c18b444273172321be600b9` |
| **new boot image SHA256** | `a5e3d8b240a204171543183141a46f58fae9222c313d743c6dc3614eab201d00` |
| kernel SHA256 (required `7dedfea6…`) | `7dedfea614d179fa81b44ecd3e8c13d5285a7267a1846f6d0631f8a74af8e263` — **MATCH** |
| initramfs SHA256 (required `0bbac457…`) | `0bbac4577f3567aec935c958216de5d30c7355452ca56248d6728f4f2634bdb6` — **MATCH** |
| binary diff vs image 02 | **86 bytes**, offsets **380–465**, all inside the header cmdline field (64–575) |
| tokens added | `ramoops.mem_address=0xc1300000`, `ramoops.mem_size=0x100000`, `ramoops.console_size=0x40000` |
| tokens removed | **NONE**; shared prefix order preserved |

Header otherwise identical: `ANDROID!`, kernel_size 48,519,996, kernel_addr
`0x8000`, ramdisk_size 4,106,247, ramdisk_addr `0x2000000`, second_addr
`0xf00000`, tags_addr `0x1e00000`, page_size 4096, header_version 1, board
`FRPSC27A006`. No `console=` added (only the stock `console=null`),
no `root=`, no `datapart=`, no `systempart=`, no USB/RNDIS params, no
DTB/DTBO, kernel-config, or initramfs changes.

## 2. Flash

```
BOOT upload successful
Rebooting device...
heimdall exit: 0
```

BOOT partition only. Post-flash verification: first 4096 B of
`/dev/block/bootdevice/by-name/boot` = `8ad7bef0dd8e131bab53aff5a334514b36364ba43c1bb6315481745f84140fd5`,
identical to the new image.

## 3. pstore result — the cmdline mechanism worked; the console record is empty

**No `console-ramoops-*` record exists.** `/sys/fs/pstore/` is completely empty —
not even the previous `pmsg-ramoops-0`.

But the region *was* configured correctly, proven from our own kernel's log:

```
[    0.217837]  ramoops: using module parameters      <-- the cmdline path
[    0.218578]  pstore: using zlib compression
[    0.219333]  console [pstore-1] enabled
[    0.219381]  pstore: Registered ramoops as persistent store backend
[    0.219391]  ramoops: attached 0x100000@0xc1300000, ecc: 0/0
```

`ramoops: using module parameters` is the `ramoops_register_dummy()` path, which
only triggers when `ramoops.mem_size` is non-zero — i.e. only our command line
could have produced it. TWRP's kernel reports `ramoops: attached
0x100000@0xc1300000` for the same region, and `/proc/iomem` still shows
`c1300000-c13fffff`.

### Why the console record is empty — and why no config change can fix it

In this Samsung 4.14.190 tree, `CONFIG_PSTORE_CONSOLE` is only a **flag**. The
console *writer* is missing:

- `fs/pstore/blk/` **does not exist** in this tree, and `fs/pstore/Makefile`
  contains no `blk.o` line. Upstream's pstore console backend is absent.
- The tree instead has a Samsung function, `fs/pstore/ram.c:721`:

  ```c
  void notrace ramoops_console_write_buf(const char *buf, size_t size)
  {
          struct ramoops_context *cxt = &oops_cxt;
          persistent_ram_write(cxt->cprz, buf, size);
  }
  ```

  `grep -rn ramoops_console_write_buf` over the whole source tree returns
  **exactly two hits: the prototype in `include/linux/pstore_ram.h:83` and this
  definition. There are no callers.** It is dead code.

So on this kernel the console zone is allocated and advertised
(`console [pstore-1] enabled`) but nothing ever writes to it. `CONFIG_PSTORE_BLK`
is not set and could not be, since the backend does not exist. **No kernel
configuration change can populate a pstore console record on this tree.** The
stock Samsung kernel evidently has the caller; ShinyKernel does not.

This also explains the earlier garbage `pmsg-ramoops-0`: TWRP's kernel does have
compression working (`pstore: using zlib compression`), so that record was
deflate-compressed. Our kernel's pmsg writer, if reached, would have had the
same problem.

## 4. The kernel log — obtained anyway, and positively identified

`/sys/fs/pstore/` is empty, but **`/proc/last_kmsg` (2 MiB) contains our boot.**
Identification is positive, not assumed:

| Evidence | Value |
| --- | --- |
| `ramoops: using module parameters` | only our cmdline produces this; the stock Android boot does not |
| `BOOT_CNT(604570165)` | the earlier Android boot had `604570161` — a later boot |
| `sec_log_store() 2018-01-23 05:51:41 ... reboot, recovery` | vs `05:03:05` on the Android boot — ~48 min later, matching the gap between our two flashes |
| `Hardware name: ... SM8150 V2 PM8150 Samsung D2Q CHN PROJECT Rev14 (DT)` | correct d2q DTB |

It is not the same buffer as last time: the timestamps, boot count, and the
`ramoops: using module parameters` line all differ, and our boot's messages start
at line 13855 of 20388. Lines before that are the older Android boot.

### What our kernel did

```
t=0.000  Linux version 4.14.190-ShinyKernel
         Machine: Qualcomm Technologies, Inc. SM8150 V2 PM8150 Samsung D2Q CHN PROJECT Rev14
t=0.218  ramoops: using module parameters          <- cmdline observability WORKED
t=0.219  ramoops: attached 0x100000@0xc1300000
t=0.625  Freeing initrd memory: 4008K
t=0.643  WARNING: CPU:1 PID:38 at kernel/rcu/tree.c:3048 __call_rcu
t=1.610  EXT4-fs (sda24): mounted filesystem without journal
t=3.550  apexd mounts 4 APEX loop images
t=5.59   sda5, sda27, sda18, sda6, sda9 mounted
t=12.738 init: Service vold has 'reboot_on_failure' option and failed,
              shutting down system.
t=12.928 init: Reboot ending, jumping to kernel
t=12.930 sec_log_store() BOOT_CNT(604570165) reboot, recovery
```

The only `WARNING` in our boot is the RCU one at t=0.643 — and it appears
**exactly once in the earlier known-good Android boot too**, so it is
pre-existing noise, not our failure.

## 5. The bootloop cause is now identified — and it is not what we expected

```
init: Service 'vold' (pid 661) received signal 9
init: Service vold has 'reboot_on_failure' option and failed, shutting down system.
```

**Stock Android userspace ran and killed itself.** `vold` is declared with
`reboot_on_failure` in Android's `init.rc`, so when it dies, Android `init`
shuts the system down and reboots into recovery. That is the bootloop.

Unmistakably Android: `init` as PID 1, `vold`, `apexd`, APEX packages mounted
from loop0–3, `/system` on **sda24**, `/data` on sda30, `/efs` on sda9, `/cache`
on sda27, `/mnt/vendor/persist` on sda5, `logd`, `servicemanager`.

## 6. The central unexplained anomaly

**Our Halium initramfs did not run.** Android's did.

Evidence that it was not ours:

- **Zero** `initrd:` markers. The Halium `scripts/halium` writes
  `initrd: Halium rootfs is …`, `initrd: checking filesystem integrity …`, and
  friends to `/dev/kmsg` via `tell_kmsg()`. All would appear in this log. None do.
- No `udevadm settle`/userdata `e2fsck`/loop-mount sequence from `mountroot()`.
- The panic telnet path never ran — **no RNDIS interface ever appeared on the
  host**, and no `18d1:d001` device was seen at any point.
- PID 1 performed Android's shutdown sequence, which the initramfs cannot do.

Evidence that our boot image *was* used:

- BOOT partition contains our image, verified byte-for-byte at offset 0.
- The kernel received **our** command line — proven by
  `ramoops: using module parameters`, which requires `ramoops.mem_size` from it.
- The bootloader logged `SEC CMDLINE` matching our image.
- `Freeing initrd memory: 4008K` equals 1002 pages of 4096 = 4,104,192 B, which
  is our 4,106,247 B ramdisk rounded down to whole pages.

The last point is the puzzle: the initrd size freed matches our ramdisk, yet the
userspace that ran was Android's. And the **stock** boot image has
`ramdisk_size = 0`, so Android's ramdisk did not come from the stock boot
partition either.

I cannot resolve this from the available evidence without further
investigation, and per instruction I am **not** changing the DTB, DTBO, kernel
config, or command line again. Flagging it as the single most important open
question.

One lead worth recording, unverified: the boot header's board name. Our image
carries `name = FRPSC27A006` while `TARGET_BOOTLOADER_BOARD_NAME := msmnile`.
If the bootloader selects a device profile from the header board name and then
substitutes its own ramdisk, that would explain this. Purely a hypothesis.

## 7. Current state

- Device in TWRP, reachable over ADB (`RF8M82H3NFM`).
- BOOT partition = `a5e3d8b2…` (the iteration-2 image).
- Rollback image `artifacts/flash/02-halium-boot-baseline-kernel.img`
  (`63d5a180…`) is intact and reflashable at any time.
- `artifacts/known-good/stock-boot.img` (`6718aba7…`) intact.
- No PIT, repartition, EFS touch, userdata wipe, CSC, bootloader, or DTBO
  operation at any point.

## 8. Logs saved

| File | Contents |
| --- | --- |
| `artifacts/device/logs/iter2-last_kmsg` | raw `/proc/last_kmsg`, our boot |
| `artifacts/device/logs/iter2-last_kmsg-cleaned.txt` | NULs stripped, 20388 lines |
| `artifacts/device/logs/iter2-logfs.img` | raw `logfs` from this boot |
| `artifacts/device/logs/iter2-UefiLog1.txt` | bootloader UEFI log, this boot |
| `artifacts/device/iter2-host-kernel.log` | host `journalctl -kf` across the boot |
| `artifacts/device/iter2-netmon.log` | host USB/link/addr transitions |

## 9. Stopping here

Per instruction, I am not attempting a fix, not changing the DTB, DTBO, kernel
config, or command line, and not flashing again. The two things I would want
authority for next, in order:

1. Determine why Android's ramdisk executed instead of the Halium initramfs.
   The cheapest next step is not a kernel change at all — it is to read what the
   bootloader actually loaded at `0x20000000`, which the UEFI log does not
   currently record, or to test the ramdisk in isolation.
2. Accept that a pstore console log is impossible on this kernel and rely on
   `/proc/last_kmsg` (which worked) as the observability channel.
