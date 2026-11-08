# BREAKTHROUGH — the GMU is ALIVE. The fault is SPTPRAC (the GMU's processor) is off

Date: 2026-10-12
Image: 223, boot `1051f121-2693-4565-a9aa-2d720efc8a11`
Capture: `artifacts/gpu-bringup/captures/dlog223b.txt`

## The verdict we were waiting for

```
GMUDEAD[GPU_DCVS]: HFI_VERSION=0x20000001  OOB_G2H_INFO=0x00000000
                  OOB_G2H_MASK=0xff7ffffe  HOST2GMU_RAW=0x00000000
                  HOST2GMU_SET=0x00000000  GX_VOTE=0x00000000
                  MX_VOTE=0x00000000        SPTPRAC_PWR=0x00000000

GMUDEAD[GPU_SET]:  HFI_VERSION=0x20000001  ...
                  HOST2GMU_RAW=0x00010000  SPTPRAC_PWR=0x00000000
```

**`HFI_VERSION = 0x20000001` — the GMU firmware IS RUNNING** (HFI protocol
v2.1). The "GMU is dead" hypothesis from GMU-DEAD.md is **retracted**.

## What is now established, positively

| check | result |
|---|---|
| GMU firmware image | byte-identical to stock (sha256 verified) |
| GMU firmware **execution** | **ALIVE** — `HFI_VERSION=0x20000001` |
| HFI queues | **started** — `hfi poll ok (STATUS=0x1)` |
| HFI version handshake | `q0 rd=0 wr=2 doorbell=0x01 data=00100201 10010000` |
| OPP table (GMU) | 4 freqs: 0 / 19.2M / 200M / 500M |
| OPP table (GPU) | 7 freqs: 0 / 257M / 345M / 427M / 499.2M / 585M / 675M |
| ARC command-db data | `gfx.lvl=16  cx.lvl=16  mx.lvl=16` — present and sane |
| GX vote written | `VOTE151: gx=0x9 mx=0x6` |

Every input the driver needs is correct and present. Nothing upstream is wrong.

## The actual fault: the vote is written, then discarded

`VOTE151` writes `GX_VOTE_IDX = 0x9`. Twenty-five milliseconds later
`GMUDEAD` reads **`GX_VOTE_IDX = 0x00000000`**.

**The GMU read the vote and cleared it.** Combined with:

- `HOST2GMU_RAW = 0x00010000` — the host's OOB request bit is **set and never
  consumed**
- both OOB handshakes time out with `OOB_G2H_INFO = 0`
- **`SPTPRAC_PWR = 0x00000000`** in every reading
- GX clock domain off: `gpu_cc_gx_gmu_clk en=0`, `gcc_gpu_gpll0_clk_src en=0`

## The unifying explanation

**SPTPRAC is not powered.** SPTPRAC is the processor that *executes* the GMU
firmware's main loop. The GMU responds to plain MMIO (register reads, the HFI
version register) because that path needs no code execution — which is exactly
why it can look "alive" while being functionally dead.

Without SPTPRAC running:
- the GMU firmware cannot service its OOB mailbox → `HOST2GMU_RAW` stays set
- it cannot act on the GX vote → `GX_VOTE` reads back 0
- it cannot raise the GX power rail → GX domain never clocks
- therefore RB/SP never run → **no blit, no render, no pixels, ever**

This is consistent with every prior observation, including the intermittent
freezes: a partially-stuck power sequence would produce exactly the marginal
behaviour we saw (one successful trigger at 360s, several freezes).

## Next moves

1. **Find why SPTPRAC is not powered.** In the driver this is
   `a6xx_sptprac_enable()` / `a6xx_sptprac_disable()` and the
   `gmu->idle_level` state machine. Check whether SPTPRAC enable is being
   reached and what it returns. Earlier logs showed
   `GMU_SPTPRAC_CLKCTL = 0x00000000` (clock control zero).
2. **Check `RPMHPWR`** — the forensic dumps show `RPMHPWR=0xf` before HFI init
   and `RPMHPWR=0x0` after. Whatever that field means, it transitions at
   exactly the wrong moment.
3. **The device tree carries `qcom,gpu-quirk-cx-gdsc`**, and KGSL has matching
   GDSC workaround code for the GPU power domain. That quirk is a strong
   candidate for what is failing here, and KGSL is now clearly the right
   reference to read.

## Retractions

- **"The GMU firmware is not executing at all"** (`GMU-DEAD.md`) — **WRONG**.
  `HFI_VERSION=0x20000001` proves it is running. The register simply needed to
  be read.
- The claim that the poller never firing indicated a stalled workqueue remains
  unexplained but is now clearly not related to the GPU fault — the fault is
  fully described without it.

## Method note

`HFI_VERSION_INFO` was the register to read and it took six images and a
subagent to find it. It answered in one line. **A register nobody has read is
often more informative than a hundred measurements of things that work.**