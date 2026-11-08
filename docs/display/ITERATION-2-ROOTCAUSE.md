# Iteration 2 — Root Cause: the Halium ramdisk was never unpacked

Investigation only. **No flash, no reboot, no write, no repartition.**
Device read-only ops used: `adb shell cat/ls/dd if=...`.

---

## Executive summary

The Halium initramfs is **not** the problem, and neither is Android FBE/userdata.
Samsung ABL hands the kernel a command line that our image does not control.
Because `skip_initramfs` is on the **effective** command line, `populate_rootfs()`
freed our 4 MB initrd **without unpacking anything** — not the external ramdisk,
and not even the kernel's own built-in stub. With no `/init` in the rootfs, the
kernel fell through to `prepare_namespace()` and mounted **`root=/dev/sda24`**
(= the stock Android **`/system`** partition) as `/`. Android's `init` therefore
ran as PID 1 straight out of `/system`.

**There was never an "Android ramdisk".** Android came from a block device that
ABL told the kernel to use as root.

---

## Answers to the 7 questions, in order

### 1. Does the flashed kernel contain a built-in initramfs?

**Yes — but it is a 512-byte stub, and it is not the cause.**

* `CONFIG_INITRAMFS_SOURCE=""` (empty), `CONFIG_BLK_DEV_INITRD=y`, `CONFIG_RD_GZIP=y`.
  `CONFIG_INITRAMFS_COMPRESSION`, `CONFIG_INITRAMFS_ROOT_UID/GID` are absent from `.config`.
* Nevertheless `ShinyKernel/Kernel/out/usr/initramfs_data.cpio` **exists**, 512 bytes,
  `ASCII cpio archive (SVR4 with no CRC)`.
* Full contents (`cpio -itv`):

  ```
  drwxr-xr-x  2 root root            0 dev
  crw-------  1 root root       5,   1 dev/console
  drwx------  2 root root            0 root
  ```

  `5,1` = `/dev/console`. **No `/init`, no Android anything.**
* This is the standard kbuild-generated stub that appears whenever
  `CONFIG_INITRAMFS_SOURCE` is empty. It is expected and harmless *by itself* —
  see §4, where it becomes decisive evidence.

### 2. If yes, does it contain an Android `/init`?

**No.** The entire built-in archive is the three entries above. No `/init`,
no `init.rc` / `init*.rc`, no `ueventd.rc`, no `fstab.*`, no `system/bin/init`.
512 bytes cannot contain them, and there is no second embedded archive:
`__initramfs_start`/`__initramfs_size` are single symbols
(`nm`/`readelf` on `artifacts/known-good/shinykernel-baseline/vmlinux`).

> Methodological note: `readelf -s` prints the *address* of `__initramfs_start`,
> not its value; the first attempt to read it as a size produced a nonsense
> 1.8e19-byte "initramfs". The value is irrelevant here because the `.cpio` file
> is the authoritative artifact.

### 3. Does the external BOOT ramdisk contain our Halium `/init`?

**Yes — verified against the on-device partition, not just our build tree.**

Header read from the live partition
(`/dev/block/bootdevice/by-name/boot`, offsets 8..31):

| field | value |
|---|---|
| header version | 0 |
| kernel_size | 48,519,996 |
| kernel_addr | 0x00008000 |
| ramdisk_size | **4,106,247** |
| ramdisk_addr | **0x02000000** |
| second_size | 0 |
| second_addr | 0x00F00000 |
| page_size | 4096 |

Layout is **sequential page-aligned**, not vaddr-mapped:

```
kernel  : file offset 0x001000 (4096)
ramdisk : file offset 0x2E47000 (48,525,312)   size 4,106,247
second  : file offset 0x3232000 (52,633,600)   size 0
file size: 52,633,600   (ends exactly after the ramdisk -> no appended DTB)
```

> Correction: `ramdisk_addr` is a *load address*, not a file offset. Reading at
> offset 0x8000 (as a first pass did) returns the **kernel**, not the ramdisk.

Pulled 4,106,247 bytes from that offset on the device:

```
sha256  0bbac4577f3567aec935c958216de5d30c7355452ca56248d6728f4f2634bdb6   MATCH
file    gzip compressed data, original size 9,120,768
```

Identical to our built ramdisk, and valid gzip.

### 4. According to the actual 4.14.190 source, which `/init` should Linux execute?

**Ours — from the external ramdisk. The kernel never got that far.**

Source, this tree, unmodified:

`init/initramfs.c:622-690`
```c
static int __init populate_rootfs(void)
{
	char *err;

	if (do_skip_initramfs) {                       /* <-- 626 */
		if (initrd_start)
			free_initrd();                  /* <-- 628  frees, prints nothing */
		return default_rootfs();            /* <-- 629 */
	}

	/* Load the built in initramfs */
	err = unpack_to_rootfs(__initramfs_start, __initramfs_size);   /* 633 */
	if (err)
		panic("%s", err);

	/* If available load the bootloader supplied initrd */
	if (initrd_start && !IS_ENABLED(CONFIG_INITRAMFS_FORCE)) {
#ifdef CONFIG_BLK_DEV_RAM                        /* =y in our config */
		int fd;
		printk(KERN_INFO "Trying to unpack rootfs image as initramfs...\n");  /* 640 */
		err = unpack_to_rootfs((char *)initrd_start, initrd_end - initrd_start);
		if (!err) {
			free_initrd();               /* 644 */
			goto done;
		} else { ... }
		printk(KERN_INFO "rootfs image is not initramfs (%s)..."
				"; looks like an initrd\n", err);                      /* 650 */
		...
#else
		printk(KERN_INFO "Unpacking initramfs...\n");            /* 668  NOT compiled */
		...
		free_initrd();                                             /* 673  NOT compiled */
#endif
	}
	...
}
rootfs_initcall(populate_rootfs);
```

`init/main.c:1456-1466`
```c
	if (!ramdisk_execute_command)
		ramdisk_execute_command = "/init";

	if (sys_access((const char __user *) ramdisk_execute_command, 0) != 0) {
		ramdisk_execute_command = NULL;
		prepare_namespace();
	}
```

So: built-in archive is unpacked **first**, the external one **second**; a
built-in `/init` could never have taken precedence (and ours has none);
`rdinit=` is unset; `ramdisk_execute_command` defaults to `/init`.
**Under a normal boot our Halium `/init` would have executed.** It did not.

### 5. Is an Android initramfs inside the kernel sufficient to explain the Android boot?

**No — decisively no, and the actual mechanism is now identified.**

#### The proof that our initrd *was* delivered

```
[    0.625192]  Freeing initrd memory: 4008K
```

* `Freeing initrd memory` is **not** a literal in our kernel. It is
  `mm/page_alloc.c:6971`, inside `free_reserved_area()`:
  ```c
  	if (pages && s)
  		pr_info("Freeing %s memory: %ldK\n",
  			s, pages << (PAGE_SHIFT - 10));
  ```
  and the vmlinux contains the format string `Freeing %s memory: %ldK`.
* On **arm64** the only caller passing `"initrd"` as that reason is
  `arch/arm64/mm/init.c:811-815`:
  ```c
  void __init free_initrd_mem(unsigned long start, unsigned long end)
  {
  	if (!keep_initrd)
  		free_reserved_area((void *)start, (void *)end, 0, "initrd");
  }
  ```
  (Verified: a tree-wide grep of `"initrd"` shows every other hit is in a
  different architecture.)
* `free_initrd_mem()` is reached only from `free_initrd()`, whose callers are
  lines **628, 644, 663, 673**. 644 and 663 both sit *after* the line-640
  `printk`; 673 is in the `#else` branch.
  **⇒ the only possible caller is line 628 — the `do_skip_initramfs` path.**

#### The proof that line 640 never ran

`Trying to unpack rootfs image as initramfs` is present **once** in the vmlinux
(and once in the `Image`), so it was compiled in — and it is a
`printk(KERN_INFO)` on the unconditional path. In the 2 MB captured log:

```
"Trying to unpack"  0
"as initramfs"      0
"not initramfs"     0
"initrd.image"      0
"initramfs"         0
"Unpacking initramfs" / "Initramfs unpacking failed" : not even in the binary
```

The log is complete and continuous across that instant
(`0.622998` → `0.625192` → `0.626859`, no gap, single boot session starting at
line 13854 / `t=0.000000`).

#### The proof that even the built-in stub was skipped

```
[    1.605447]  Warning: unable to open an initial console.        <- console_on_rootfs()
[    1.610136]  New pool created id:0
[    1.610349]  EXT4-fs (sda24): mounted filesystem without journal. Opts: (null)
[    1.610413]  VFS: Mounted root (ext4 filesystem) readonly on device 259:8.
```

`Warning: unable to open an initial console.` is `console_on_rootfs()` failing
to `filp_open("/dev/console")`. `/dev/console` **is** in the built-in stub. So
the built-in archive was never unpacked either — exactly what lines 626-630
dictate, and what no other path allows (a `mknod` failure in `do_copy()` is
silent, but then line 640 would still have printed).

#### The size arithmetic ties it to *our* ramdisk

```
4,106,247 bytes / 4096 = 1002.5 pages
free_reserved_area: start = PAGE_ALIGN(start), end = end & PAGE_MASK
=> 1002 whole pages = 4,104,192 bytes = 1002 << (12-10) = 4008K   <- exact match
```

#### And the root it mounted

`prepare_namespace()` needs `root=`. It was provided — **by Samsung ABL**:

```
artifacts/device/logs/iter2-last_kmsg:13665-13667
    dm-verity cmdline Append!!
    DstLen 4213, SrcLen 50
    AVB CMD LINE [root=PARTUUID=97d7b011-54da-4835-b3c4-917ad6e73d74]
```

> **CORRECTED in iteration 3.** This block previously cited
> `iter2-UefiLog1.txt` / `iter2-logfs.img` and read `root=/dev/sda24`. That file
> is a stale artifact written by a **different bootloader build**
> (`N9750ZSU6HXE1`, not our `N9750ZSU6HZE1`). ABL refuses to refresh it
> (`BDS: LogFs sync skipped, Unsupported`). The authoritative source is the ABL
> block inside `iter2-last_kmsg`, which belongs to our boot. The root
> filesystem is still stock `/system` — confirmed independently at runtime.
> See `ITERATION-3-CMDLINE-RECONSTRUCTION.md`.

Identified on-device:

```
/proc/partitions : 259  8  6492160  sda24
/dev/block/bootdevice/by-name/system -> /dev/block/sda24
```

**6,492,160 KB ≈ 6.2 GB — the stock Android `/system`.**
That is where `init`, `vold`, `apexd`, the APEX loop mounts and
`reboot_on_failure` all came from.

### 6. Evidence specifically pointing to Samsung ABL replacing the external ramdisk?

**Important correction: ABL did not replace or substitute our ramdisk.**
The bytes at the header's `ramdisk_addr` region on the device are byte-identical
to ours (§3) and the kernel freed exactly that region (§5). Our ramdisk was
delivered, and the kernel discarded it.

What ABL does control is **the command line**, and that is where the failure is:

* `CONFIG_CMDLINE=""`, and **neither** `CONFIG_CMDLINE_FORCE` **nor**
  `CONFIG_CMDLINE_EXTEND` is set. So in `drivers/of/fdt.c:1190-1240`:
  ```c
  #else /* CMDLINE_FROM_BOOTLOADER */
  static const int overwrite_incoming_cmdline;   /* 0 */
  static const int read_dt_cmdline = 1;           /* 1 */
  static const int concat_cmdline;                /* 0 */
  #endif
  ...
  	if (read_dt_cmdline)
  		p = of_get_flat_dt_prop(node, "bootargs", &l);
  	if (p != NULL && l > 0) {
  		if (concat_cmdline) { ...append... }
  		else {
  			strlcpy(cmdline, p, min((int)l, COMMAND_LINE_SIZE));
  		}                                  /* <-- REPLACES */
  	}
  ```
  **The DTB's `/chosen/bootargs` overwrites the boot-image header command line
  entirely.** On arm64 DT-boot that bootargs string *is* the command line.
  The 402-character `BOARD_KERNEL_CMDLINE` we packed is only a **seed** that
  ABL expands.

* ABL demonstrably builds the runtime DTB itself:
  ```
  Decompressing kernel image total time: 542 ms
  Override DTB: GetBlkIOHandles failed loading user_dtbo!
  EDTBO check fail
  Apply Overlay total time: 367 ms
  Final Dtb version = 0
  kaslr-Seed is added to chosen node
  Update Device Tree total time: 139 ms
  ```
  It also patches `/chosen` for KASLR (`arch/arm64/kernel/kaslr.c:34-42` reads
  `/chosen/kaslr-seed`) and `early_init_dt_scan_chosen()` reads
  `/chosen/linux,initrd-start|end` for the initrd window.

* Our BOOT image carries **no DTB** (`second_size = 0`, file ends exactly after
  the ramdisk), so ABL sourced the base DTB + overlays itself.

**Where `skip_initramfs` comes from — the one remaining unverified link:**

* **Not** our image: the packed header cmdline is 402 chars, no `skip_initramfs`.
* **Not** the on-flash `dtbo` partition: all **15** DTBO entries were extracted
  and parsed (`magic 0xd7b7ab1e`, header 32 B, entry 32 B, count 15,
  total 5,086,794). Every entry is a valid FDT (`d00dfeed`) of ~360 KB, and
  **every single one has `chosen` = absent and `bootargs` = absent**. Whole-partition
  literals: `chosen` 0, `bootargs` 0, `skip_initramfs` 0.
* **Not** ABL's logged appends — but our capture cannot prove completeness. ABL's
  log appends, in order, reconstructed exactly from its own `DstLen` accounting
  (`4296 - 31 - 5 - 24 - 1 - 19 - 2 - 16 = 4198`):
  ```
  +31  ' androidboot.verifiedbootstate='
  + 5  'green'
  +24  ' androidboot.keymaster=1'
  + 1  ' '
  +19  ' androidboot.ulcnt='
  + 2  '0 '
  +16  ' root=/dev/sda24'
  ```
  No `skip_initramfs`. **However** `iter2-UefiLog1.txt` is a fixed 102,400-byte
  ring and its cmdline prints are demonstrably damaged: the `Orig :` line is
  byte-interleaved (`...vendor/firmw` + `accic_check_active`) and the
  `SEC CMDLINE :` line is hard-capped at 511 chars, ending mid-token at
  `androidboot.`. A whole-partition search of `iter2-logfs.img` (8 MB) for
  `skip_initramfs` returns 0.
  ⇒ **`skip_initramfs` must be in the bootargs ABL writes into the runtime DTB,
  but the token itself has not been read directly from a device.**

* **The board-name hypothesis is dead.** `FRPSC27A006` vs
  `TARGET_BOOTLOADER_BOARD_NAME := msmnile` had no observable effect: the kernel
  and the 4,106,247-byte initrd were both consumed exactly as the header
  described. Recording this so it is not re-litigated.

### 7. Is Android userdata / FBE relevant?

**No. The failure happens strictly before userdata is ever consulted.**

Actual order of events on the device:

```
t=0.000  Booting Linux on physical CPU 0x0 / Linux version 4.14.190-ShinyKernel
t=0.217  ramoops: using module parameters          <- our cmdline seed WAS used
t=0.219  ramoops: attached 0x100000@0xc1300000
t=0.625  Freeing initrd memory: 4008K              <- our ramdisk discarded, not unpacked
t=1.605  Warning: unable to open an initial console  <- rootfs is empty
t=1.610  VFS: Mounted root (ext4) readonly on 259:8 <- /system, per ABL's root=
   ...   Android init as PID 1, vold, apexd, APEX loop mounts
   ...   init: Service vold has 'reboot_on_failure' option and failed
```

This is case **A** from the brief — Android was selected as the initial
userspace, *before* Halium ever got control. Our Halium `init`
(`/tmp/opencode/initrd-x/init`) and `scripts/halium` **never executed a single
line**: no `initrd:` markers, no `mountroot()` sequence, no panic RNDIS/telnet
gadget, no `18d1:d001`. FBE/userdata is a downstream *consequence* of booting
stock Android as root, not a cause.

---

## What this rules out

| Suspect | Verdict |
|---|---|
| Halium initramfs content / `/init` | **Exonerated** — byte-identical on device, valid gzip |
| Built-in initramfs holding Android userspace | **Exonerated** — 512-byte stub, no `/init` |
| ABL substituting the ramdisk at `0x02000000` | **Exonerated** — our bytes are there and were freed |
| `system.img` required | **Exonerated** (unchanged from `PRE-FLASH-ANALYSIS.md`) |
| Android userdata / FBE | **Exonerated** — failure precedes userdata |
| Board name `FRPSC27A006` vs `msmnile` | **Dead hypothesis** |
| **`skip_initramfs` on ABL's `/chosen/bootargs`** | **Cause** — deduced from source + 4 independent log facts; the literal token not yet read from the device |

## BUILD-VERIFIED vs RUNTIME-VERIFIED

* **BUILD-VERIFIED** (host-side, this session): built-in initramfs contents; all
  config symbols; source of `Freeing %s memory: %ldK`; the full arm64 call graph
  to `free_initrd()`; `early_init_dt_scan_chosen()` replace-vs-concat semantics;
  the absence of `Unpacking initramfs` from the binary; all 15 DTBO entries and
  their missing `/chosen`; the 4008K arithmetic; ABL's `DstLen` append ledger.
* **RUNTIME-VERIFIED** (measured on the device this session): on-device boot
  header fields; on-device ramdisk sha256 == our ramdisk; `system -> sda24` =
  259:8 = 6.2 GB; on-device absence of `skip_initramfs`/`bootargs`/`chosen` in
  the whole `dtbo` partition; the log lines quoted above (log capture is
  read-only).
* **NOT VERIFIED** — do not treat as fact: the literal presence of the token
  `skip_initramfs` inside ABL's runtime `/chosen/bootargs`. What is proven is
  the *behaviour*: `populate_rootfs()` took the `do_skip_initramfs` branch, and
  `do_skip_initramfs` has exactly one setter in the whole tree
  (`init/initramfs.c:617`, `__setup("skip_initramfs", ...)`). Since it is a
  `__setup` parameter it can only come from the command line, and the command
  line on this device is ABL's `/chosen/bootargs`.

## Next step (needs authorization, nothing done)

The remaining gap is *observability of ABL's bootargs*, not a kernel change:

1. Capture a **complete, uncorrupted** ABL cmdline — re-read the `logfs`
   partition right after a single clean boot, and de-interleave the ring
   (`B -`/`D -` markers make ordering recoverable), or read all four `UefiLogN`
   slots. That should print the full `SEC CMDLINE` including any
   `skip_initramfs` and the tail past 496 chars.
2. If `skip_initramfs` is confirmed, the remedy is a **command-line** remedy
   (whatever makes ABL emit a bootargs without it), not a kernel, ramdisk, or
   partition change. Whether ABL honours an override needs to be established
   before anything is flashed.
3. `BOARD_KERNEL_CMDLINE` should not be treated as authoritative until (1) is
   done — on this device it is only ABL's input seed.

## Files referenced

* `artifacts/device/logs/iter2-last_kmsg`, `iter2-last_kmsg-cleaned.txt` — kernel log
* `artifacts/device/logs/iter2-UefiLog1.txt`, `iter2-logfs.img` — ABL/UEFI log
* `artifacts/device/ondevice/ramdisk-ondevice.gz` — ramdisk read off the device
* `artifacts/device/ondevice/dtbo-full.img` — whole `dtbo` partition
* `artifacts/known-good/shinykernel-baseline/{vmlinux,Image}` — the kernel that ran
* `ShinyKernel/Kernel/init/initramfs.c` (611-690), `init/main.c` (1456-1466),
  `mm/page_alloc.c` (6955-6982), `arch/arm64/mm/init.c` (811-815),
  `drivers/of/fdt.c` (1190-1240), `arch/arm64/kernel/kaslr.c` (34-42)
* `n9750-halium/device/samsung/d2q/BoardConfig.mk:67` — `BOARD_KERNEL_CMDLINE`
