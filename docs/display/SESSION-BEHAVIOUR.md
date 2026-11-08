# Odin/Download Mode session behaviour — observed evidence

Device: Samsung SM-N9750 / d2q, VID:PID `04e8:685d`, product string `SM8150`.
Host: CachyOS, kernel `7.2.7-1-cachyos`.
Tools: heimdall v2.2.2, odin4 7.3.0-a46321b (native Linux, by Llucs).

No partition, PIT, bootloader, EFS or userdata write was performed at any point.
Every command below is read-only.

## Result summary

| # | Time (local) | Command | Device node | Result |
| --- | --- | --- | --- | --- |
| 1 | 19:08 | `lsusb` | n/a | enumerated, `1-3` product `SM8150` |
| 2 | ~19:09 | `heimdall detect` | 001/012 | `Device detected` |
| 3 | ~19:09 | `heimdall print-pit` | 001/012 | **SUCCESS** — session begun, PIT downloaded, 76 entries, tag `SM8150`, `Device Type: 8 (UFS)` |
| 4 | ~19:10 | `heimdall print-pit` | 001/012 | `ERROR: Protocol initialisation failed!` |
| 5 | ~19:11 | `odin4 -l` | 001/012 | `/dev/bus/usb/001/012` |
| 6 | ~19:14 | `odin4 --check-only -s <boot.img>` | 001/012 | `Starting handshake` → `USB handshake failed.` after 52 s |
| 7 | 19:23 | cable reconnected, Download Mode re-entered | 001/013 | device re-enumerated 012 → **013** |
| 8 | 19:23 | `heimdall detect` | 001/013 | `Device detected` |
| 9 | 19:23 | `heimdall print-pit` | 001/013 | **SUCCESS** — `Protocol initialisation successful`, `Session begun` |
| 10 | 19:24 | `heimdall print-pit` | 001/013 | `ERROR: Protocol initialisation failed!` |
| 11 | 19:23 | `odin4 -l` | 001/013 | `/dev/bus/usb/001/013` |

## The pattern

**Exactly one session-consuming Odin command succeeds per Download Mode entry.**

- Entry 1 (node 012): `print-pit` succeeded once (step 3), then every subsequent
  session command failed (steps 4, 6).
- Entry 2 (node 013): `print-pit` succeeded once (step 9), then failed again
  (step 10).

Enumeration-only commands (`lsusb`, `heimdall detect`, `odin4 -l`) never consume
the session and always work.

## What this rules out

- **Not permissions.** ACL on the node grants `user:usb:rw-`; `os.open(O_RDWR)`
  succeeds; `heimdall detect` claims the interface successfully.
- **Not a Heimdall bug.** `odin4` is an independent implementation of the Odin
  protocol and fails at the same stage (step 6), after enumerating interface 1,
  EP IN `0x81`, EP OUT `0x01`, wMaxPacketSize 512.
- **Not enumeration.** The device is present in `lsusb` and via sysfs at all
  times, at 480 Mbit/s, with correct VID/PID/product string.
- **Not cable or port.** A successful session is achieved on each fresh entry.

## What remains

The device accepts one Odin protocol session, then stops responding to
subsequent session establishment until Download Mode is re-entered. Whether this
is a bootloader-side session limit, a host-side endpoint state that survives the
process exit, or a quirk of this bootloader build is **not established**. The
"dirty session after PIT dump" explanation is consistent with the evidence but is
not proven, and other causes (for example a one-shot session counter in the
bootloader, or an endpoint state that a reconnect of the *host* side would also
clear) have not been excluded.

## Operational consequence

Because a reconnect costs the user physical effort and yields a single session,
the session must be spent on the operation that actually matters. Read-only
inspection commands (`print-pit`, `--check-only`) **consume** it.

For this reason the remaining work was moved to host-only analysis: inspect the
boot image, trace the initramfs, and determine the userdata write path, all
without touching the device.
