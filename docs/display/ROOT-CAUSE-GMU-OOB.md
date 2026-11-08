# ROOT CAUSE: the GMU never acknowledges the OOB that should power the GX rail

Date: 2026-10-12
Evidence: `/sys/kernel/debug/clk/clk_summary`, live dmesg, `a6xx_gmu.c`
Source: image 221 boot `26271774`, kernel `/home/usb/ShinyKernel-gpu-exp/Kernel`

## The complete chain, verified at every link

1. **`clk_summary` — the GX (graphics) clock domain is off.**
   ```
    gpu_cc_cx_gmu_clk      en=1 prep=1  rate=200000000    CX  (CPU/context) ON
    gpu_cc_gx_gmu_clk      en=0 prep=0  rate=200000000    GX  (graphics)    OFF
    gcc_gpu_gpll0_clk_src  en=0 prep=0  rate=600000000    GPU core PLL      OFF
   ```
   CX runs the CP; GX runs the RB/SP/UCHE, i.e. everything that writes pixels.

2. **`GXVOTE=0x0` in the GMU** — the GX vote register reads zero, polled every
   30 s by the existing `GVIT157` diagnostic:
   ```
   [1789.919729] GVIT157: SPTPRAC=0x0 RPMHPWR=0x0 GXVOTE=0x0 MXVOTE=0x0 ...
   ```

3. **The vote is *written*, though.** `a6xx_gmu_gfx_rail_on()` (a6xx_gmu.c:843)
   does:
   ```c
   vote = gmu->gx_arc_votes[gmu->nr_gpu_freqs - 1];
   gmu_write(gmu, REG_A6XX_GMU_GX_VOTE_IDX, vote & 0xff);
   gmu_write(gmu, REG_A6XX_GMU_MX_VOTE_IDX, (vote >> 8) & 0xff);
   ```
   and captures from image 212 show it running:
   ```
   VOTE151: gx=0x9 mx=0x6 (writes only, no OOB)
   ```
   So the write happens and the register reads back 0 → **the GMU is not acting
   on the vote.**

4. **THE ACTUAL FAILURE — the OOB handshake times out.**
   ```
   [1195.183215] POST_START: oob GPU_SET ack-wait enter
   [1195.193215] POST_START: oob GPU_SET ack-wait TIMEOUT (INTR_INFO=0x10000000)
   ```
   From `a6xx_gmu_set_oob()` (a6xx_gmu.c:732-745):
   ```c
   gmu_write(gmu, REG_A6XX_GMU_HOST2GMU_INTR_SET, 1 << request);
   ret = gmu_poll_timeout(gmu, REG_A6XX_GMU_GMU2HOST_INTR_INFO, val,
                          val & (1 << ack), 100, 10000);
   ```
   The host requests `GMU_OOB_GPU_SET` (power up the GPU / GX rail) and waits
   10 s for the GMU to acknowledge. **It never does.** 10 ms later it times out.

   `INTR_INFO=0x10000000` is a single stray bit, not the expected ACK — the GMU
   is alive enough to respond to MMIO but is not servicing the OOB request.

## Why this is the root cause

`GMU_OOB_GPU_SET` is the mechanism by which the **GMU firmware raises the GX
power rail**. The GMU firmware is what actually programs the RPCS/ARC commands
that switch on the GPU's GX clocks. If that handshake never completes:

- GX domain stays unclocked → `clk_summary` shows it off
- The GX vote is never honoured → `GXVOTE=0`
- CP (CX domain) keeps running → ring fetches, `CP_EVENT_WRITE`, fence all work
- RB/SP/UCHE never run → **no blit, no render, no pixels, ever**

That is precisely the observed symptom set, and it explains why *every* earlier
hypothesis failed: the commands were always correct, the addresses were always
correct, the buffers were mapped — the hardware that executes writes was never
turned on.

It also explains why `a6xx_hw_init` diffing against donor v4.19 found nothing:
the register sequences are faithful, but the missing step is a *firmware
handshake*, not a register write.

## Why the handshake fails — the open question

Candidate causes, in rough order:

1. **The GMU firmware (a640_gmu.bin) is not running or is stale.** If the GMU
   itself is wedged, it will not service OOB. We have previously confirmed the
   SQE (`a630_sqe.fw`) is byte-identical to stock, but **the GMU binary has not
   been verified the same way.** Worth doing immediately — it is cheap.
2. **Wrong boot-slumber / OOB sequencing.** The driver writes
   `GMU_BOOT_SLUMBER_OPTION=0` then the vote, then issues the OOB. The order and
   the `legacy` flag interact (see the `gmu->legacy` split at :1310). If the GMU
   is already out of slumber when we poke it, the OOB may be ignored.
3. **GMU firmware version mismatch with the host driver.** The CAF msm driver
   expects a specific GMU interface revision; if the firmware in
   `vendor.raw.img` is a different build, OOB handling can differ silently.

## Next steps, in order

1. **Verify `a640_gmu.bin`** from `vendor.raw.img` — size, and compare against
   any stock copy. Cheap, and it is the single most likely cause.
2. **Read the GMU's version/state registers** at the point of the timeout, and
   check whether the GMU firmware is actually executing (compare with the SQE
   `sqe_pc` check that worked for the SQE).
3. **Try issuing the OOB before/without the vote writes**, and try the
   `legacy` path, to see whether any ordering produces an ACK.
4. Only then consider force-enabling `GPU_CC_GX_GMU_CLK` from the kernel as a
   workaround — that would probably work but papers over the real fault and may
   leave the GPU without correct power/voltage ramping.

## Retractions this supersedes

- "GPU clocks are mis-programmed" (`CLKCHK` internal registers) — they are fine;
  the gate above them is shut by the GMU, not by us.
- "Nested IB fetch broken" — proven false.
- "The clear is emitted incorrectly" — proven false.
- "Render target unmapped (`0x26000`)" — proven false; it is `0x25000+0x1000`.

## Method note

Five images of kernel instrumentation were spent chasing the command stream.
The fault was never in the command stream. It was one poll timing out on a
firmware handshake, visible in a log line that had been printed all along
(`ack-wait TIMEOUT`), and in a clock summary nobody had thought to read.

**Before instrumenting a driver hard, enumerate the platform: clocks, power
domains, regulators, firmware handshakes.** The driver was doing its job
correctly the whole time.