# IMAGE 222 — flashed; phone froze on GPU trigger at 385s uptime

Date: 2026-10-12
Image: `artifacts/flash/222-halium-boot-a6xx-gmulive.img`
SHA256: 0a0ff54502d850050ec315bcf4fa53964f56c500e21f728d17a91a40081e8015
Boot: `7730a930-d14c-49ca-b468-f12dd12fb9ab`

## What was done

- Flash: clean, BOOT-only, hash verified, ramdisk identical to 221.
- Boot `7730a930`, responsive.
- **dmesg banked at uptime 157s (8181 lines) BEFORE the trigger**, while the
  kernel ring buffer was still clean — this was deliberate, because ring
  wrapping has twice cost us the bring-up window. It is at
  `/root/222-preboot.txt` on the device (not yet pulled to host).
- Waited out the `gpu_init_delay_ms=300000` deferral: triggered at uptime
  **385s**, past the 300s window that previously froze.
- Trigger returned `TRIGGER_OK`; the phone then stopped answering ARP
  (`100%` loss, `ip neigh` PROBE/FAILED) while `lsusb` still enumerated
  `18d1:d001` and the link stayed UP.

## Correction to the "wait past 300s" rule

Earlier (image 221) triggering at ~360s worked and ~70–90s froze, which led me
to conclude "trigger only after the deferral expires". **That rule is
insufficient.** Image 222 triggered at 385s — comfortably past 300s — and still
froze.

So the deferral boundary is not the whole story. Revised understanding:

- Triggering **well inside** the window (~70–90s): reliably freezes.
- Triggering **past** it (360s on 221, 385s on 222): worked once, froze once.

Something else is marginal — plausibly a race between the GMU OOB handshake
(which we know times out) and the power sequence, with the freeze a symptom
rather than the root cause. The `GMULIVE` reading is still the right next
measurement; we just have not been able to collect it on this boot.

## What we did not get

The `GMULIVE` line never ran — it is emitted by the 30-second `GVIT157` poller
inside bring-up, and the phone wedged during the trigger before it could be
read. `GMULIVE` count was 0 pre-trigger (expected, poller not yet running).

**The device-side `/root/222-preboot.txt` (8181 lines, captured pre-trigger)
should be pulled on the next successful boot before anything else**, since it
contains the full bring-up window with no ring-buffer wrap.

## Still to measure (unchanged)

`REG_A6XX_GMU_HFI_VERSION_INFO` (0x5181) — the GMU's own version handshake
reply, never yet read:

| value | meaning |
|---|---|
| real version | GMU firmware is running → OOB sequencing is the bug |
| `0x00000000` | GMU firmware is not executing at all |
| `0xffffffff` | GMU registers not responding (bus/MMIO issue) |

Alongside it, image 222 dumps `GMU2HOST_INTR_INFO` (did the GMU ever send the
OOB ack), `GMU2HOST_INTR_MASK`, `HOST2GMU_INTR_RAW_INFO`, `HOST2GMU_INTR_SET`,
`GX_VOTE_IDX`, `MX_VOTE_IDX`, `SPTPRAC_PWR_CLK_STATUS`.

## Firmware status (from GMU-FW-VERIFIED.md, still valid)

`a640_gmu.bin` and `a630_sqe.fw` on the device are **byte-identical to the
stock blobs** in `vendor.raw.img`. The firmware image is not the problem.