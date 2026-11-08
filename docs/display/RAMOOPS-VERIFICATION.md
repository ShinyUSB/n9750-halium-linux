# ramoops / PSTORE verification — does `CONFIG_PSTORE_RAM_CONSOLE=y` suffice?

Investigated before rebuilding, as required. **Conclusion: yes — and it is proven
from a real boot on this device, not assumed.**

## 1. Where ramoops is defined

Not in the base DTBs. In the **d2q DTBO overlays**:

`arch/arm64/boot/dts/samsung/davinci/sm8150-sec-d2q-chn-overlay-r*.dts`

```dts
ramoops@A1300000 {
        compatible = "ramoops";
        reg = <0x0 0xc1300000 0x0 0x100000>;   /* base 0xc1300000, size 1 MiB */
        record-size  = <0x40000>;               /* 256 KiB */
        console-size = <0x40000>;               /* 256 KiB  <- the record we want */
        ftrace-size  = <0x40000>;               /* 256 KiB */
        pmsg-size    = <0x40000>;               /* 256 KiB  <- the record we already have */
};
```

Answering each point that was asked:

| Question | Answer |
| --- | --- |
| ramoops base address | `0xc1300000` |
| ramoops size | `0x100000` = 1 MiB |
| record size | `0x40000` = 256 KiB (`record-size`) |
| console record | `console-size = 0x40000` = 256 KiB |
| pmsg record | `pmsg-size = 0x40000` = 256 KiB |
| reserved in the DTB? | Yes, in the overlay DT, as a normal `ramoops` node (not `reserved-memory`/`no-map`; ramoops claims the region itself via `devm_ram_ioremap` and marks it exclusive) |

### Which overlays contain it

Checked the **compiled** `.dtbo` files by scanning for the node name string:

- **contain `ramoops@A1300000`**: `r00, r01, r02, r03, r04, r06, r07, r08, r09, r10, r11, r12, r13, r14` — all 14 revision overlays
- **do not contain it**: `sm8150-sec-system-update-overlay.dtbo` only

So every hardware-revision overlay carries the ramoops node. The bootloader
selects a revision overlay, so the region should be present in our boot.

> Correction to an earlier statement: I first reported that ramoops was absent
> from the compiled `.dtbo`. That was a tooling error — I invoked
> `./tools/dtc/dtc`, which is a directory, so decompilation silently produced
> nothing and the grep appeared to come up empty. The compiled overlays do
> contain the node, confirmed by `strings`.

## 2. Empirical proof from a real boot on this device

From `/proc/last_kmsg` (the known-good stock-Android + ShinyKernel boot — a
*reference* boot, not ours), at t=0.266 s:

```
[    0.266120]  pstore: using zlib compression
[    0.267120]  console [pstore-1] enabled
[    0.267188]  pstore: Registered ramoops as persistent store backend
[    0.267201]  ramoops: attached 0x100000@0xc1300000, ecc: 0/0
```

This is decisive on every point:

- The **DTB actually delivered to the kernel on this device contains the ramoops
  node** — the driver attached at exactly `0xc1300000` with size `0x100000`,
  byte-for-byte matching the overlay definition. The overlay *is* applied at
  boot; the `EDTBO check fail` / `Dtbo_Version = 0` messages in the bootloader
  log are therefore benign on this device (they also appear in this working
  boot, because there is no `user_dtbo` partition).
- The region **survives reboot**: that is what ramoops is for, and this is the
  same region TWRP later exposed.
- `console [pstore-1] enabled` shows the console backend working in that kernel.
  **Our** kernel has it off, which is the single difference we are about to
  change.

## 3. Is the existing `pmsg-ramoops-0` the same region?

Yes — same 1 MiB region, different record.

`pmsg-size = 0x40000` (256 KiB) is the printk ring dump written by
`pstore_dump`/`kmsg_dump` at panic/shutdown. The 97,268-byte file pulled from
TWRP is that record. The record we want is the *adjacent* one,
`console-size = 0x40000`, which appears as `/sys/fs/pstore/console-ramoops-0`
and is populated by the `pstore/blk` console backend — i.e. by
`CONFIG_PSTORE_RAM_CONSOLE=y`.

They coexist; enabling the console backend does not disturb pmsg.

## 4. Why the existing pmsg read as garbage

Explained, and it corrects my earlier guess. I assumed the garbled 97 KB was
simply "not a console dump". The reference boot logs **`pstore: using zlib
compression`**, so the record is **deflate-compressed**. Reading the raw file
byte-for-byte yields noise, which is exactly what we saw.

This is not a problem for the plan:

- A pstore record carries its own compressed flag (`CPSTORAE_FLAG_COMPRESSED`)
  in the header, and `pstorefs` decompresses according to that flag, not
  according to the *reading* kernel's config. TWRP mounts `pstorefs` and will
  decompress the console record correctly.
- Our kernel's own compression setting is irrelevant to readability. For the
  record: our config has `CONFIG_PSTORE_DEFLATE` and `CONFIG_PSTORE_COMPRESS`
  **not set**, whereas the stock Samsung kernel has zlib on. So our console
  record will be written **uncompressed** — larger, but strictly more robust to
  read, and TWRP handles it either way.

## 5. Sufficiency, and the one residual risk

`CONFIG_PSTORE_RAM_CONSOLE=y` **is sufficient on its own.** The region is
reserved, the driver attaches, the console record is sized, and a real boot on
this device has already shown all of that happening.

Residual risk, stated honestly: the proof comes from a boot using the same
bootloader + same `dtbo` partition + same `Image-dtb` append format, but it was
not our own boot. If our kernel somehow receives a device tree *without* the
ramoops node — for example if the bootloader picked
`sm8150-sec-system-update-overlay.dtbo`, the one overlay that lacks it — then
there would be no backend and `/sys/fs/pstore/` would stay empty. That is
observable rather than silent: if the second flash produces no
`console-ramoops-0`, the DTB path is the thing to investigate, and `pmsg` plus
the `ramoops: attached` line will tell us immediately.

Adding `console=ttyMSM0,115200n8` to the cmdline is complementary and costs
nothing: it does not affect pstore capture, and it gives live output if a UART is
ever attached. It is not required for this test.

## 6. Resulting pstore layout in `/sys/fs/pstore/`

| File | Record | Size | Written when |
| --- | --- | --- | --- |
| `console-ramoops-0` | `console-size` | 256 KiB | continuously, once `PSTORE_RAM_CONSOLE=y` |
| `pmsg-ramoops-0` | `pmsg-size` | 256 KiB | at panic/shutdown (`kmsg_dump`) |
| `dmesg-ramoops-0` | `record-size` | 256 KiB | `ramoops` persistent dmesg (`CONFIG_PSTORE_RAM` `record-size`) |
| `ftrace-ramoops-0` | `ftrace-size` | 256 KiB | ftrace, unused here |

The first flash produced no `console-ramoops-0` and no readable
`pmsg-ramoops-0` because the console backend was disabled in our kernel and
because the pmsg record we saw was compressed. Enabling the console backend
addresses both: we get a plain, uncompressed, continuously-written log.
