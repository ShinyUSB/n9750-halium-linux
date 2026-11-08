# IMAGE 228 — the forced gate write WEDGES the system. The GX clock domain is genuinely not ready.

Date: 2026-10-12
Image: `artifacts/flash/228-halium-boot-a6xx-forcclk2.img`
sha256: `368ed3d926896b884f436eab1aede49d125fb872dfd69df635305817d2bb0a9b`
Boot: `bafc378d`, kernel `#34`
Status: **device froze on trigger; manual reboot required**

## What happened

Image 227 Oops'd before reaching the forced-gate block (my `of_iomap` bug), so
the experiment had never actually run. Image 228 fixed the mapping and executed
it. The device **froze on the GPU trigger**:

```
trigger sent at uptime 337s
ssh: connect to host 10.15.19.82 port 22: Connection timed out
u.sh --fix: link still down after 6 attempts
ping 10.15.19.82: 100% loss, 8 consecutive attempts over ~3 minutes
lsusb: 18d1:d001 still enumerated
ip addr show enp0s20f0u3: 10.15.19.100/24 present  <- host IP intact
```

This is **not** an RNDIS IP wipe — the interface still holds its address and the
device is still on the bus. It is the real freeze signature recorded in
`IMAGE-221-CLOCKS.md` / `IMAGE-223-FROZE.md`.

## Why this is a result, not just another lost image

Images 225, 226 and 227 all completed bring-up and stayed alive. Image 228 is
the first one that actually **performed the forced
`gpu_cc_gx_gmu_clk` gate write**, and the first one to freeze.

`IMAGE-76-RESULTS.md` warned about exactly this, and the warning was correct:

> "the same DT node carries a power vote we are not making […]] Enabling clocks
> inside a still-collapsed domain is a plausible way to wedge a clock-gating
> register write."

So the forced write confirms the diagnosis rather than relieving it: **the GX
clock domain cannot simply be switched on. Something upstream of it genuinely
has not completed.** If the rail and clocks were merely "not yet enabled", a
host-side gate write would be harmless; instead it wedges the interconnect.
The GPU is not merely idle — its power/clock-enable sequence is incomplete, and
the firmware has stopped partway through it.

That is a real narrowing, and it points away from "the firmware is fine, nobody
asked" toward "the firmware asked and the sequence stalled mid-way".

## What is still unmeasured

`GXCLK228: forced ... 0x… → 0x…` never reached the host — the device was frozen
before it could be read. The on-device logger was running and wrote
`/root/dlog228.txt`; `RESUME.md` records that `/root` has survived reboots in
this project, so that file should contain the `GXCLK228` line and the state at
the moment of the wedge.

**First action after the manual reboot: pull `/root/dlog228.txt` before
re-triggering anything.**

## Standing state (unchanged since image 226)

| layer | state |
|---|---|
| CM3 boot, HFI bootstrap | ✅ all `error=0` |
| GX vote `GX_BW_PERF_VOTE` | ✅ acked 3× |
| OOB `GPU_SET` v2 bits 30/31 | ✅ acked, `INTR_INFO=0x80000000` |
| stuck OOB request / bit-28 mirror | ✅ now zero |
| TCS/RPMh transaction | ✅ `TCS0=0x1`, `RSC0=0x1000000` |
| GX GDSC host enable | ✅ ret=0, and **irrelevant** |
| GPU PLL | ✅ `gpll0 en=2` 600 MHz |
| **`gpu_cc_gx_gmu_clk`** | ❌ `en=0`, and **forcing it wedges the system** |
| fence / pixels | ❌ |

## What this rules out and what it promotes

**Ruled out:** that the gate is merely gated and the rest of the sequence is
healthy. A one-bit gate write is not enough to wedge a working domain.

**Promoted:** the AOP/QMP ACD notification. It is the only remaining step
between "GMU firmware issues a vote" and "the power/clock sequence actually
runs to completion", it is present in **both** reference implementations
(KGSL `gmu_aop_send_acd_state()` and upstream msm `qmp_get`/`qmp_send`), and it
has never been attempted in 228 images. `ANALYSIS-GX-POWER-REVIEW.md` §2 has had
this open since it was written.

**Blocked on:** CAF's exact ACD packet format, which I could not source. This
tree has `CONFIG_MAILBOX=y` and `drivers/mailbox/msm_qmp.c` driving the
`qmp_aop` node, so it is implementable — but it must be ported from real source,
not invented. Inventing a protocol value is the documented failure mode of this
project (`CONSULT-MUSE-SPARK-CP-IRQ.md`: two consultants, two confident wrong
answers, both from guessing).

## Note on the wasted cycle

Image 227 produced no data at all: `of_iomap()` returned a garbage pointer on
this 4.14 tree and I faulted `a6xx_gmu_resume`. Three build failures on that one
change, all from guessing a DT API instead of reading `include/linux/of_address.h`
first. Image 228 then compiled first try once the API was read properly.

Recorded because it cost a flash cycle, and because the same mistake — assert an
API from memory instead of reading the tree — is the single most expensive habit
in this project.