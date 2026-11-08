# GMU firmware verified byte-identical to stock — that hypothesis is DEAD

Date: 2026-10-12

## What was checked

Extracted both GPU firmware blobs from the **stock vendor partition**
(`/home/usb/mnt-sda/a6xx-build/fw/vendor.raw.img`, ext2, via `debugfs`) and
compared against the copies actually present on the running device.

```
STOCK vendor.raw.img:/firmware/a640_gmu.bin    37680 bytes
STOCK vendor.raw.img:/firmware/a630_sqe.fw     32304 bytes

DEVICE /lib/firmware/qcom/a640_gmu.bin         37680 bytes
DEVICE /lib/firmware/qcom/a630_sqe.fw          32304 bytes
```

SHA256:

| blob | sha256 | verdict |
|---|---|---|
| `a640_gmu.bin` (stock) | `3ff0c02708bbe78641db887fa62f3a7f9337934d0c2ce0b961ef7c43172591d2` | — |
| `a640_gmu.bin` (device) | `3ff0c02708bbe78641db887fa62f3a7f9337934d0c2ce0b961ef7c43172591d2` | **IDENTICAL** |
| `a630_sqe.fw` (stock) | `a0e1b583f620fabe32729ce367959d1960638663244d7d0cfc21b9a5215a018b` | — |
| `a630_sqe.fw` (device) | `a0e1b583f620fabe32729ce367959d1960638663244d7d0cfc21b9a5215a018b` | **IDENTICAL** |

## Conclusion

**The GMU firmware image is not the problem.** It is the exact stock blob from
this device's own vendor partition. This retires the top suspect from
ROOT-CAUSE-GMU-OOB.md.

The failure therefore narrows to one of:

1. **The GMU firmware is not executing at all** — loaded correctly but never
   runs, so it never services the OOB request.
2. **OOB/boot-slumber sequencing** — the host pokes the GMU in an order it
   ignores (the `gmu->legacy` split at `a6xx_gmu.c:1310` and the
   `BOOT_SLUMBER_OPTION=0` write interact).
3. **The GMU is running but wedged** on something earlier in bring-up.

## Next measurement (image 222)

The decisive question is simply: **is the GMU alive?**

`REG_A6XX_GMU_HFI_VERSION_INFO` (0x5181) is the GMU's own version handshake
reply. We have never read it.

- Returns a real version (non-zero, non-`0xffffffff`) → GMU firmware is
  running; the OOB sequencing is the bug.
- Returns `0x00000000` → GMU firmware is not running at all.
- Returns `0xffffffff` → the GMU registers are not responding (bus/MMIO issue).

Also dumped alongside it, from the existing 30-second `GVIT157` poller:
`GMU2HOST_INTR_INFO` (pending GMU→host interrupts — should show the OOB ACK
if the GMU ever sent one), `GMU2HOST_INTR_MASK`, `HOST2GMU_INTR_RAW_INFO`,
`HOST2GMU_INTR_SET`, `GX_VOTE_IDX`, `MX_VOTE_IDX`, `SPTPRAC_PWR_CLK_STATUS`.

This reuses the existing periodic worker, so it adds no new scheduling risk and
no new printk volume per submit.

## Note on earlier captures

The `GVIT157` poller and the `POST_START: ack-wait TIMEOUT` line were present in
**every** capture from image 212 onward and were never read as the answer. The
infrastructure was already printing the root cause; we were looking at the
command stream instead.

Also: dmesg ring-buffer wrapping has repeatedly cost us the bring-up window
(earliest surviving entry is ~t=457 s while bring-up happens ~t=360 s). The
per-submit `BOS[]` dump at an 8192-dword cap is the cause. Image 222 adds only a
30-second periodic line, so this is unchanged — but the next image that touches
the dump should narrow it.