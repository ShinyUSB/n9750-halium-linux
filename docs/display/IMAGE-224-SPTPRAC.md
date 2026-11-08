# IMAGE 224 — SPTPRAC fix attempted; the GDSC does not respond to the register write

Date: 2026-10-12
Image: `artifacts/flash/224-halium-boot-a6xx-sptprac.img`
Boot: `49915c0c-02f3-4fbc-995e-b352b8b6ebcc`

## Result: the fix is well-founded but insufficient

```
SPTPRAC224: before PWR_CLK_STATUS=0xc0
SPTPRAC224: after request PWR_CLK_STATUS=0xc0      <- write had NO effect
SPTPRAC224: poll TIMEOUT status=0xc0
```

Everything downstream is unchanged:

```
POST_START: oob GPU_DCVS ack-wait TIMEOUT (INTR_INFO=0x0)
GMUDEAD[GPU_DCVS]: HFI_VERSION=0x20000001 ... SPTPRAC_PWR=0x00000000
gpu_cc_gx_gmu_clk   en=0  rate=200000000             <- still OFF
GXSTATE: SPTPRAC=0x0 GDSC_OFF=0 CLK_OFF=0 RPMHPWR=0x0 GXVOTE=0x0 MXVOTE=0x0
```

## What this tells us — and it is genuinely new

The `gmu_rmw()` write to `REG_A6XX_GMU_SPTPRAC_PWR_CLK_STATUS` **does not
change the register at all**: `0xc0` before, `0xc0` after.

Reading `0xc0` against the bit definitions:
```
0x40 = GX_HM_GDSC_POWER_OFF
0x80 = GX_HM_CLK_OFF
```

So **GX_HM — the graphics hardware block — is powered off, and its clock is
off.** And the SPTPRAC bits (0x01–0x10) are all zero: not powering-on, not on,
not off. The register is not latching our command.

This is the first direct evidence that the **GDSC is not accepting host-driven
power sequencing at all**, rather than that we asked for it incorrectly. Our
bit-level reasoning (POWERING_ON 0x02, poll POWER_ON 0x04) matches CAF; the
hardware simply does not respond to it.

That points squarely at the device-tree property we have been carrying but never
acted on:

```dts
qcom,gpu-quirk-cx-gdsc;
```

in `sm8150-gpu.dtsi`'s `qcom,kgsl-3d0` node. This is a **GPU power-domain
(GDSC) quirk flag**. KGSL has corresponding workaround code that our msm/drm port
does not implement — we have no GDSC workaround machinery at all
(`grep devm_clk adreno/*.c` → no matches; nothing in the tree handles it).

`qcom,gpu-quirk-secvid-set-once` is the sibling quirk, and the tree does have
some SECVID handling.

## Revised picture

| layer | state |
|---|---|
| bring-up, CP, command stream, IB fetch | **working** |
| GMU firmware | **running** (`HFI_VERSION=0x20000001`) |
| HFI queues | **started** |
| GX_HM (graphics block) power | **OFF** — `0xc0`, unresponsive to host writes |
| SPTPRAC | never powered, GDSC will not respond |
| GX clock domain | **OFF** |
| pixels | never produced |

Everything above the power domain works. **The GPU's graphics hardware block is
never powered on, and the GDSC does not respond to the standard SPTPRAC power
sequence.** That is the single remaining cause, and it is a *platform/firmware*
problem, not a driver-command problem.

## Next moves, now much better targeted

1. **Read KGSL's `cx-gdsc` workaround** and port it. This is now clearly the
   required path: KGSL handles this quirk and we do not. It is the first time
   the KGSL comparison has had a concrete, specific target rather than a vague
   "look at KGSL's init".
2. **Check whether the DT property is even reaching the driver.** Our node is
   `qcom,kgsl-3d0` (KGSL's compatible string) while msm/drm binds on
   `qcom,adreno`. Verify which properties our driver can actually read.
3. `RPMHPWR=0x0` at every sample — whatever that field reports, it never
   indicates the GPU is powered. Worth comparing against a device where KGSL
   runs.

## Honest note on the fix

The image 224 change was **correct** — the old code polled the wrong bit
(`POWER_OFF` 0x08 instead of `POWER_ON` 0x04) and had been disabled entirely by
image 198. But the register not latching the write shows the problem is deeper
than the bit we were polling. The fix was a genuine bug fix, just not the last
one.

Worth noting too: this run did **not** freeze on the GPU trigger, unlike the
previous several. That may be coincidence, but it is worth watching.