# Correction: `CONFIG_PSTORE_RAM_CONSOLE` does not exist in this kernel

I was asked to enable only `CONFIG_PSTORE_RAM_CONSOLE=y`. While verifying that
it would actually work, I found that **the symbol does not exist in Linux
4.14.190**, and that the symbol which does the job was **already enabled** in the
baseline we shipped. I stopped before building or flashing a boot image, because
the requested change is a no-op and proceeding would have burned a flash on an
unchanged kernel.

## What I found

`fs/pstore/Kconfig` in this tree defines exactly these pstore symbols:

```
config PSTORE
config PSTORE_ZLIB_COMPRESS
config PSTORE_LZO_COMPRESS
config PSTORE_LZ4_COMPRESS
config PSTORE_CONSOLE
config PSTORE_PMSG
config PSTORE_PMSG_SSPLOG
config PSTORE_FTRACE
config PSTORE_RAM
```

There is no `config PSTORE_RAM_CONSOLE`. `grep -rn PSTORE_RAM_CONSOLE fs/pstore/`
returns nothing. That symbol was introduced in later kernels; in 4.14 the pstore
console backend is simply `CONFIG_PSTORE_CONSOLE`.

**And `CONFIG_PSTORE_CONSOLE=y` was already set**, in both trees we have been
shipping:

```
$ grep PSTORE out/.config          # the baseline behind flashed image 02
CONFIG_PSTORE=y
CONFIG_PSTORE_ZLIB_COMPRESS=y
CONFIG_PSTORE_CONSOLE=y            <-- already enabled
CONFIG_PSTORE_PMSG=y
CONFIG_PSTORE_PMSG_SSPLOG=y
CONFIG_PSTORE_RAM=y
```

The Halium profile (`out-halium/.config`) is identical on these symbols.

## What the build actually produced

I ran the authoritative `build.txt` procedure into a fresh `out-pstore/` tree,
merging `configs/d2q_observability.config` (which contains only
`CONFIG_PSTORE_RAM_CONSOLE=y`), then `olddefconfig`.

`merge_config.sh` + `olddefconfig` silently discarded the unknown symbol, so:

```
$ diff out/.config out-pstore/.config
(no output)   -> configuration is byte-for-byte identical
```

Resulting artifacts:

| | baseline `out/` | rebuild `out-pstore/` |
| --- | --- | --- |
| `Image-dtb` size | 48,519,996 | 48,519,996 (same) |
| SHA-256 | `7dedfea614d179fa81b44ecd3e8c13d5285a7267a1846f6d0631f8a74af8e263` | `493855f120412fdd1ceeb8d7df2797ecbcbc3fbab4f614509c4481ce48ee5823` |
| config diff | — | none |
| differing bytes | — | 1158 |

The only difference is the embedded build timestamp in the `uts` banner:

```
baseline : #1 SMP PREEMPT Fri Sep 25 17:54:21 +03 2026
rebuild  : #1 SMP PREEMPT Fri Sep 25 20:09:17 +03 2026
```

So the rebuild is a functionally identical kernel. It is **not** worth flashing.

## Correction to my earlier diagnosis

In the first-flash report I wrote that `CONFIG_PSTORE_RAM_CONSOLE` "is not set"
and that this was one of the reasons no kernel log existed. **That reasoning was
wrong**, in two ways:

1. The symbol does not exist, so grepping for it and finding nothing told me
   nothing about whether the console backend was enabled.
2. The real symbol, `CONFIG_PSTORE_CONSOLE`, was `=y` all along.

The other two reasons from that report still stand: `console=null` means printk
goes to no console device, and the ring buffer is lost on reboot. But pstore was
never the disabled part.

## Where that leaves the diagnosis

If `CONFIG_PSTORE_CONSOLE=y` was already in the flashed kernel, then the
absent `/sys/fs/pstore/console-ramoops-0` after the first flash is **not**
explained by a config symbol. The remaining explanation is that **the ramoops
region was never attached in our boot**, i.e. our kernel did not receive a device
tree containing the `ramoops@A1300000` node.

That is consistent with everything observed:

- the bootloader logs `EDTBO check fail`, `Final Dtb version = 0`,
  `Dtbo_Version = 0`, and `Override DTB: GetBlkIOHandles failed loading user_dtbo!`
- yet the known-good stock-Android boot on this device *did* attach ramoops at
  `0xc1300000`, and it shows the same `EDTBO check fail` messages

so the overlay selection is the thing that differs between our boot and the
working Android boot, and it is the natural next thing to investigate. I cannot
confirm it without a kernel log, which is the problem being solved.

## The way out that needs no DTB change and no kernel rebuild

`fs/pstore/ram.c` exposes the region as boot-time parameters:

```
line 60: module_param_hw(mem_address, ullong, other, 0400);
line 65: module_param(mem_size, ulong, 0400);
```

The `other` flag on `mem_address` means these are settable **from the kernel
command line**, not only as module parameters. So:

```
ramoops.mem_address=0xc1300000 ramoops.mem_size=0x100000
```

on the cmdline makes ramoops self-configuring, independent of whether the
device tree carries the node. That breaks the chicken-and-egg: no DTB change, no
kernel rebuild, just a boot image rebuild with a different command line.

This **exceeds the current authorization**, which permits only
`console=ttyMSM0,115200n8` to be added to the cmdline. I am not doing it
unilaterally.

## Options, and what each costs

| Option | Change | Gets us a kernel log? |
| --- | --- | --- |
| A | flash the rebuilt-but-identical kernel | **No** — same config as image 02, would bootloop again identically |
| B | cmdline += `ramoops.mem_address=0xc1300000 ramoops.mem_size=0x100000` | **Yes, most likely** — no kernel rebuild, no DTB change; needs cmdline authorization |
| C | cmdline += `console=ttyMSM0,115200n8` only | Only with a physical UART adapter on `ttyMSM0`; useless without one |
| D | B + C together | Yes, and additionally gives live UART output if a serial adapter is ever attached |
| E | investigate the DTB/DTBO overlay path directly | Understands the root cause, but cannot be observed without a log first — chicken and egg |

My recommendation is **B**, optionally with C.

I am not going to flash anything, and I have not touched the recovery image.
