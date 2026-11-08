# Iteration 2: build report and pre-flash verification

Authorized change: add `ramoops.mem_address=0xc1300000` and
`ramoops.mem_size=0x100000` to the kernel command line, rebuild one BOOT image
from the existing image-02 baseline, flash BOOT only.

## 1. Prerequisites verified before building

### 1a. Address and size come from stock-Android runtime evidence

Three independent sources agree exactly:

| Source | Value |
| --- | --- |
| d2q DTBO overlay `ramoops@A1300000 { reg = <0x0 0xc1300000 0x0 0x100000>; }` | base `0xc1300000`, size `0x100000` |
| stock Android boot on this device, `/proc/last_kmsg` t=0.267: `ramoops: attached 0x100000@0xc1300000, ecc: 0/0` | same |
| `/proc/iomem` on the live device: an isolated `c1300000-c13fffff : System RAM` | exactly `0x100000` |

The iomem entry is a discrete 1 MiB hole, bounded below by
`c0000000-c10fffff` and above by `c1c01000-17fffffff`, so the region is
reserved and not allocated to anything else. The kernel has no
`CONFIG_SMT`/ECC complication: the stock boot reported `ecc: 0/0`.

### 1b. Both are valid boot-time parameters in this exact source

`fs/pstore/ram.c`:

```c
59: static unsigned long long mem_address;
60: module_param_hw(mem_address, ullong, other, 0400);
64: static ulong mem_size;
65: module_param(mem_size, ulong, 0400);
```

Kernel documentation for this tree, `Documentation/admin-guide/ramoops.rst:61`,
gives a command-line example using the same parameter:

```
mem=128M ramoops.mem_address=0x8000000 ramoops.ecc=1
```

### 1c. The built ramoops accepts these when `PSTORE_RAM=y`

`ramoops` is built **into** the kernel, not as a module — `fs/pstore/Makefile`
has `ramoops-objs += ram.o ram_core.o`, and the build produced
`out-pstore/fs/pstore/ramoops.o` with no `ramoops.ko`. Built-in parameters are
parsed from the kernel command line.

Critically, the cmdline path does **not** require a device-tree node.
`fs/pstore/ram.c`:

```c
919: static void ramoops_register_dummy(void)
920: {
921: 	if (!mem_size)
922: 		return;
923:
924: 	pr_info("using module parameters\n");
...
932: 	dummy_data->mem_size = mem_size;
933: 	dummy_data->mem_address = mem_address;
...
952: 	dummy = platform_device_register_data(NULL, "ramoops", -1, ...);
...
956: static int __init ramoops_init(void)
957: {
958: 	ramoops_register_dummy();
959: 	return platform_driver_register(&ramoops_driver);
960: }
961: postcore_initcall(ramoops_init);
```

So the kernel synthesises a `ramoops` platform device from the cmdline values at
`postcore_initcall`, entirely independent of the DT. This is the property that
makes the experiment meaningful: if the DTBO overlay's `ramoops@A1300000` node
never reaches the kernel, the cmdline path still attaches. If it *does* reach
the kernel, the real node wins and the cmdline values are ignored — also fine.

The probe guard at `fs/pstore/ram.c:761` will pass, because the record-size
parameters are not zero:

```c
761: if (!pdata->mem_size || (!pdata->record_size && !pdata->console_size &&
762: 		!pdata->ftrace_size && !pdata->pmsg_size)) {
763: 	pr_err("The memory size and the record/console size must be non-zero\n");
```

### 1d. No kernel configuration change

None was made. `out/.config` is untouched; the merged fragment
`configs/d2q_observability.config` contained only the non-existent
`CONFIG_PSTORE_RAM_CONSOLE`, so `olddefconfig` discarded it and the resulting
`out-pstore/.config` is byte-for-byte identical to the baseline `out/.config`.
The existing symbols are sufficient:

```
CONFIG_PSTORE=y
CONFIG_PSTORE_RAM=y
CONFIG_PSTORE_CONSOLE=y
```

## 2. The one risk I have to flag before flashing

`ramoops` allocates its records from the region using the size parameters,
which **all default to `MIN_MEM_SIZE` = 4096** when not given on the cmdline:

```c
#define MIN_MEM_SIZE 4096UL
static ulong record_size         = MIN_MEM_SIZE;   /* 4096 */
static ulong ramoops_console_size = MIN_MEM_SIZE;  /* 4096 */
static ulong ramoops_ftrace_size  = MIN_MEM_SIZE;  /* 4096 */
static ulong ramoops_pmsg_size    = MIN_MEM_SIZE;  /* 4096 */
```

I was authorized to add only `mem_address` and `mem_size`, so the console
record will be **4 KiB**, not the 256 KiB the device tree would have provided
(`console-size = <0x40000>`).

Consequence: the console record holds roughly the last 4 KiB of kernel output —
about 40–60 lines. This vendor kernel emits thousands of lines during driver
init, so 4 KiB will contain the messages immediately before whatever kills the
boot. That is genuinely the most relevant window, and if the kernel dies early
it will contain the earliest messages instead. But it is 64x smaller than what
the DT would give, and if the failure is not at the very tail of the log, the
record may not contain it.

The region is 1 MiB and only 4 x 4 KiB = 16 KiB would be used, so there is
plenty of room. Setting `ramoops.console_size=0x40000` (and optionally
`ramoops.pmsg_size=0x40000`) would use it — **but that is a third and fourth
parameter and was explicitly not authorized.**

I am not adding them unilaterally. Since there is only one authorized flash,
this is the decision point.

## 3. Build result

Boot image rebuilt through the established path
(`tools/build-halium.sh --boot-only` → `m mkbootimg` + `mka halium-boot`).

The kernel used is the **original baseline** `out/arch/arm64/boot/Image-dtb`,
chosen deliberately so the kernel payload is byte-identical to image 02 and the
cmdline is the only variable. The `out-pstore/` rebuild was not used: it has an
identical config and differs only in the embedded build timestamp.

## 4. Pre-flash report

| Item | Value |
| --- | --- |
| **old boot image SHA256** (on device now) | `63d5a180a9615afa70e10a1411b3a86c02ea16f14c18b444273172321be600b9` |
| **new boot image SHA256** | `72ed53e8f03adea40d3515dcf269948e30a32488d6ff0f7c7e92eb3522889fb5` |
| new image path | `artifacts/flash/03-halium-boot-baseline-kernel-ramoops-cmdline.img` |
| image size (both) | 52,633,600 bytes |
| **kernel SHA256** (both) | `7dedfea614d179fa81b44ecd3e8c13d5285a7267a1846f6d0631f8a74af8e263` |
| **initramfs SHA256** (both) | `0bbac4577f3567aec935c958216de5d30c7355452ca56248d6728f4f2634bdb6` |

### Exact command line, before and after

**Before** (image 02, currently on the device):

```
console=null androidboot.hardware=qcom androidboot.console=ttyMSM0 androidboot.memcg=1 lpm_levels.sleep_disabled=1 video=vfb:640x400,bpp=32,memsize=3072000 msm_rtb.filter=0x237 service_locator.enable=1 swiotlb=2048 androidboot.usbcontroller=a600000.dwc3 firmware_class.path=/vendor/firmware_mnt/image loop.max_part=7
```

**After** (new image):

```
console=null androidboot.hardware=qcom androidboot.console=ttyMSM0 androidboot.memcg=1 lpm_levels.sleep_disabled=1 video=vfb:640x400,bpp=32,memsize=3072000 msm_rtb.filter=0x237 service_locator.enable=1 swiotlb=2048 androidboot.usbcontroller=a600000.dwc3 firmware_class.path=/vendor/firmware_mnt/image loop.max_part=7 ramoops.mem_address=0xc1300000 ramoops.mem_size=0x100000
```

Diff: `+ " ramoops.mem_address=0xc1300000 ramoops.mem_size=0x100000"`. Nothing
removed or reordered.

### Confirmation that the only intentional change is the two ramoops parameters

- **kernel payload: byte-identical.** Same SHA-256 as image 02. No kernel
  rebuild difference is present in this image at all.
- **initramfs payload: byte-identical.** Same SHA-256 as image 02.
- **whole-image binary diff: 57 bytes**, at offsets **380–436**, which is
  entirely inside the header command-line field (offsets 64–575). No byte
  outside that field differs.
- **all other header fields identical:** `ANDROID!`, kernel_size 48,519,996,
  kernel_addr `0x8000`, ramdisk_size 4,106,247, ramdisk_addr `0x2000000`,
  second_addr `0xf00000`, tags_addr `0x1e00000`, page_size 4096, header_version
  1, board `FRPSC27A006`.
- **unchanged, as required:** no `console=` added, no `root=`, no `datapart=`,
  no `systempart=`, no USB/RNDIS parameters, no DTB/DTBO change, no kernel
  config change, no initramfs change, no `rndis_bam` change, no partition
  layout change, no boot header parameter change.

### Planned flash

BOOT partition only. No PIT, no repartitioning, no EFS, no userdata wipe, no
CSC, no bootloader, no DTBO, no other partition.

### Recovery path preserved

- `artifacts/flash/02-halium-boot-baseline-kernel.img` — the image currently on
  the device; flashing it again reverts this iteration exactly
- `artifacts/known-good/stock-boot.img` — the original Samsung image
- `artifacts/known-good/shinykernel-baseline/Image-dtb` — the known-good kernel

## 5. What I expect, and how I will read the result

Success looks like `/sys/fs/pstore/console-ramoops-0` existing in TWRP, plus
`ramoops: attached 0x100000@0xc1300000` and `using module parameters` in its
contents. I will identify records by content and timestamp and will **not** treat
the pre-existing `pmsg-ramoops-0` as evidence from this boot unless its contents
or timestamps establish that.

If no `console-ramoops-*` appears, I stop, as instructed. I will not change the
DTB, DTBO, kernel config, or command line again.
