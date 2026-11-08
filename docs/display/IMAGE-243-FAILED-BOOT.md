# Image 243 — flashed, phone fell back to recovery (TWRP) — ROOT CAUSE FOUND

**Root cause: a corrupt ramdisk produced by my packaging script, not the kernel
changes.** See "The real bug" below. The earlier revision of this document
blamed the flash state and cleared the code; that was wrong.

## The real bug

`/tmp/opencode/pack243.sh` extracted the ramdisk from image 242 at the wrong
offset:

```python
f.seek(ps); rd = f.read(rs)     # BUG: ps == 4096 == kernel start
```

The ramdisk does **not** start at `page_size`. With `kernel_size = 48418114`
and `page_size = 4096` the kernel occupies `11821` pages, so the ramdisk
starts at:

```
4096 + ceil(48418114 / 4096) * 4096  =  4096 + 48418816  =  48422912
```

My file was therefore **4096 bytes of kernel tail followed by the ramdisk with
its first 4096 bytes missing**. It still decompressed as *something*, so
nothing complained. Detectable in one glance: the file began with the literal
ASCII `UNCOMPRESSED_IMG` instead of the gzip magic `1f 8b`.

`UNCOMPRESSED_IMG` is the marker the kernel build writes into
`arch/arm64/boot/Image-dtb-hdr` — seeing it at offset 0 of a "ramdisk" is proof
the offset was wrong.

Correct extraction, and the sha `RESUME.md` already documented:

```
ramdisk offset 48422912, length 4117936
sha256 eb5d2eb53d176ea1c6f72db0bc23b6727447202f23d086982928fd0f83e1be7c
gzip decompresses to 9149440 bytes
```

`RESUME.md` says, verbatim: *"Verify the ramdisk sha (`eb5d2eb5…`) before
packaging."* The value was in a document I had already read at the start of the
session. I re-derived the offset by hand instead of using it, and my faulty
derivation overrode the documented fact. The pack script now hard-fails on the
sha.

## How the evidence actually resolved it

The rollback was the decisive experiment, and it exonerated the code:

| kernel | source | result |
|---|---|---|
| 242 original artifact | 242 | **boots Debian** |
| 243 | 242 + five parity changes | TWRP |
| 242-rollback | 242 source, fully reverted | TWRP |

Diffing the known-good kernel against the rollback kernel: **34 differing bytes
out of 48,418,114** — build timestamp, `#49`/`#52` localversion, and the
20-byte GNU build-id note. The kernels were functionally identical, so the code
changes could not have been responsible.

Diffing the two *boot images* then located it immediately:

```
242-good.img      @48422912 : 1f 8b 08 00 ...   gzip   <- real ramdisk
242-rollback.img  @48422912 : "UNCOMPRESSED_IMG"          <- kernel tail
```

48422912 is the kernel→ramdisk boundary. 14,939 differing regions in that image
all traced back to this one bad offset.

## Why the earlier "verification" passed

Three checks were run against the on-device BOOT partition after 243 flashed,
and all three confirmed the corrupt ramdisk had been faithfully written:

- header bytes — identical
- kernel payload region pulled from `sda20` — byte-identical, `GBIF243` string
  present at the expected offset
- ramdisk region sha256 — **matched the host's corrupt `ramdisk242.bin`**

They proved the flash was faithful. They could not prove the ramdisk was
correct, because the thing being compared against was itself wrong. Verifying
a build against the build that produced it is circular; the only independent
reference was the sha in `RESUME.md`.

## Corrections to the earlier revision of this document

- "Packaging was verified correct" — **wrong.** The three checks were circular.
- "Nothing in 243 runs before the trigger, so the diff cannot affect boot" —
  correct, and it was in fact right. The failure was the ramdisk.
- "Most likely the first flash was in an abnormal state because `print-pit`
  and `detect` were run first" — **wrong**, and it sent the investigation down
  the wrong path for two flash cycles.

## Two process lessons

1. **Never re-derive a value that is already written down.** `RESUME.md` had
   the ramdisk sha. Use it.
2. **`cp -a` preserves mtimes, so restoring sources does not trigger a
   rebuild.** The first rollback build silently reused stale `.o` files and
   produced a binary still containing all 243 code, while reporting success and
   a correct `.config` hash. Always `touch` restored sources, and grep the
   resulting kernel for a marker you expect to be *gone*.

## State after the fix

`artifacts/flash/242-rollback.img`, sha256
`06e99846a1c9bd21ebe7967ffd5539da896589f7804ecee9938b341cdc6b5f3a`

- ramdisk sha verified against `RESUME.md`
- differs from the known-good 242 by **57 bytes total**: 23 in the `id` field
  and 34 in the kernel payload

The phone was left running the known-good 242 artifact. Image 243 needs
repackaging with the corrected ramdisk before it is retried.