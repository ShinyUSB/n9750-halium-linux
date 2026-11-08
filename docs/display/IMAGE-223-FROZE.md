# IMAGE 223 — flashed; phone froze on trigger, but the GMUDEAD data should be on-device

Date: 2026-10-12
Image: `artifacts/flash/223-halium-boot-a6xx-gmudead.img`
SHA256: 35f1670cac94b50a127053e8d61cf5a824cad54f052706f61462dffdad8a0886
Boot: `793e16bb-9428-4918-bfac-98fe658b95e4`

## Run

- Flash clean, BOOT-only, hash verified, ramdisk identical to 222.
- Boot `793e16bb`, responsive at 158s uptime.
- `dmesg > /root/223-preboot.txt` — 8282 lines banked before the trigger.
- **Background logger started BEFORE the trigger** (the technique that worked on
  the last good run):
  `while true; do dmesg > /root/dlog223.txt; sleep 2; done`
- Waited out the 300s deferral; triggered at **360.75s**.
- Trigger returned `TRIGGER_OK`; the phone then went unresponsive (ARP FAILED,
  100% loss, `18d1:d001` still enumerated, link UP).

**Same freeze signature as before.** The phone needs a manual reboot.

## Data captured on-device, needs pulling

| file | contents |
|---|---|
| `/root/223-preboot.txt` | 8282 lines, pre-trigger, ring buffer clean |
| `/root/dlog223.txt` | 2-second-interval `dmesg` snapshot during/after the trigger |

`/root/dlog223.txt` is the one that matters — it should contain `GMUDEAD[...]`
with `HFI_VERSION`, printed **inline at the OOB timeout**, which is a code path
we have already proven executes. `/root` has survived reboots in this project,
so it should still be there.

**First action on the next boot: pull both files before doing anything else.**
Do not re-trigger the GPU until `GMUDEAD` has been read.

## The verdict we are waiting for

`REG_A6XX_GMU_HFI_VERSION_INFO` (0x5181) is written only by the GMU firmware
itself — nothing host-side writes it.

| value | meaning |
|---|---|
| real version | GMU firmware IS running → the fault is our OOB / boot-slumber sequencing |
| `0x00000000` | GMU firmware is NOT executing at all |
| `0xffffffff` | GMU register block not responding (bus/MMIO) |

## Standing knowledge

- `a640_gmu.bin` and `a630_sqe.fw` on the device are byte-identical to stock
  (verified via sha256 against `vendor.raw.img`).
- GX (graphics) clock domain is OFF; CX (CPU/context) is ON. CX runs the CP,
  GX runs the RB/SP — which is why the command stream works and nothing is ever
  written.
- Both OOB requests (`GPU_DCVS`, `GPU_SET`) time out with `INTR_INFO=0x0`.
- `a6xx_hw_init` is faithful to donor v4.19 (nothing missing).
- Deferred works armed from `a6xx_gmu_resume()` never execute
  (`KEEPALIVE156` also 0), though workqueues work elsewhere on the system. This
  is why `GMUDEAD` was placed inline rather than in the poller.