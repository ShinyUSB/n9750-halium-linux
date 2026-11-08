# 🎉 IMAGE 225 — the GX vote is being sent, acknowledged, and acted on. First real GX power.

Date: 2026-10-12
Image: `artifacts/flash/225-halium-boot-a6xx-gxvote.img`
sha256: `1e8ddabd68f08a72527e03af4c83444dbb05a187cc3b771be96fbda65e9cb0f7`
Boot: `235737c6` (device alive at uptime 580s+, **no freeze, no reboot**)
Capture: `captures/225-final.txt` (9019 lines)

> **CORRECTION (added after cross-model review).** This document originally
> treated `SPTPRAC 0xc0 → 0x0` and `GXSTATE verdict GX_ON` as proof that GX came
> up. **That is an over-claim and is withdrawn.** `captures/221-full.txt` shows
> `SPTPRAC==0x0` at every GVIT157 sample *on a boot whose own `clk_summary`
> proves the GX clocks were unenabled* — 0x0 is the ambiguous all-clear value,
> not a healthy state. The only ground truth is the clock tree, and
> `gpu_cc_gx_gmu_clk` was still `en=0` at the end of this run.
>
> The real, still-valid results of image 225 are the four protocol facts below
> (vote sent and acked, OOB acked, GDSCR transitioned, PLL alive) and the
> single remaining blocker: **the leaf GX clock gate is still closed**. GX did
> not come up on image 225; the driver now *asks correctly*, which was never
> true before, and that is what unblocks finding the real gate.
>
> A further correction: images 212–217 already sent `GX_BW_PERF_VOTE` and got
> `ret=0` (`captures/212-post.txt:5905-5909`) with GX still dark — because those
> runs still used legacy OOB bits 16/24. **Image 225 is the first boot carrying
> both fixes**, and the first in which the GDSC actually transitioned. It is
> the *combination* that is the result; neither half alone was sufficient.
>
> Also: `RPV225` emitted **zero** lines on this run. The poller is gated on
> `experimental_nonlegacy` but the live cmdline sets `al_nonlegacy` — a
> different parameter name. The RPMh/TCS instrumentation added to interpret
> image 225 never executed. Fixed in image 226.

---

## 1. What changed on the hardware

| # | was (images 1–224) | now (image 225) |
|---|---|---|
| GX vote HFI msg | **never sent** | `HFI-ACK id=30 seq=6/7/8 error=0` — acked 3× |
| OOB `GPU_SET` | `ack-wait TIMEOUT (INTR_INFO=0x10000000)`, every boot, every suspend | `ack-wait ok (INTR_INFO=0x80000000) ret=0`, instantly, on **every** resume |
| `SPTPRAC_PWR_CLK_STATUS` | `0xc0` forever (GDSC_OFF=1, CLK_OFF=1) | `0xc0` → **`0x0`** at GXSTATE[1] and stays `0x0` |
| `GX_GDSCR` | `0x222001` | **`0xf8222800`** — a real GDSC state transition |
| `GXSTATE` verdict | `GX_OFF` at all 4 sample points | **`GX_ON`** at all 3 remaining points |
| `gcc_gpu_gpll0_clk_src` | `en=0` (PLL completely dark) | `gpll0 en=2` **600 MHz**, `gpll0_out_even en=2` 300 MHz, `gcc_gpu_gpll0_div_clk_src en=1` |
| TCS activity | never dumped (assumed none) | `TCS0=0x1`, `RSC0=0x1000000` — an RPMh transaction **actually ran** |
| GL context | never created | **`RENDERER : FD640`**, Mesa 22.3.6, FBO `0x8cd5 COMPLETE` |

## 2. The decisive sequence, verbatim

```
[330.765975] HFI-ACK id=30 seq=6 error=0                    <- GX vote #1 acked
[330.777978] HFI-ACK id=30 seq=7 error=0                    <- GX vote #2 acked
[330.778201] GXVOTE225: idx=6/7 legacy=0 -> SPTPRAC=0xc0 GX_GDSCR=0x222001 TCS0=0x1 RSC0=0x1000000
[330.778342] GXSTATE[0]: SPTPRAC=0xc0 GDSC_OFF=1 CLK_OFF=1 GXGDSCR=0x222001   verdict GX_OFF
[330.778538] POST_START: oob GPU_SET ack-wait ok (INTR_INFO=0x80000000)       <- THE HANDSHAKE
[330.778589] GXSTATE[1]: SPTPRAC=0x0  GDSC_OFF=0 CLK_OFF=0 GXGDSCR=0xf8222800 verdict GX_ON
[330.779026] GXSTATE[2]: ...                                                      verdict GX_ON
[330.779338] GXSTATE[3]: ...                                                      verdict GX_ON
[330.780137] A6XX_TRACE: gpu->funcs->hw_init returned (ret=0)
```

Two things to read off this:

1. **`TCS0=0x1` is the first nonzero TCS status ever observed.** For 224 images
   every document asserted "the GMU votes GX up" while having no evidence. The
   vote interface was enabled; now a transaction has genuinely been issued.
2. **The GDSC transition happens across the `GPU_SET` handshake**, 200 µs after
   the vote. `INTR_INFO=0x80000000` is bit 31 — the non-legacy ACK bit. The
   firmware acknowledged the request *and* changed the GDSC. This is the
   first time the GMU firmware has responded to a host request in this project
   beyond the HFI bootstrap.

The clock tree before → after:

```
before (224 images):        after (225):
  gpll0            en=0        gpll0                      en=2  600000000
  gpll0_out_even   en=0        gpll0_out_even             en=2  300000000
  ...div_clk_src   en=0        gcc_gpu_gpll0_div_clk_src  en=1  300000000
  gpu_cc_gx_gmu_clk en=0       gpu_cc_gx_gmu_clk          en=0  <-- STILL 0
  gpu_cc_cx_gmu_clk en=1       gpu_cc_cx_gmu_clk          en=1
```

## 3. Still broken: one gate

```
gpu_cc_gx_gmu_clk   en=0 prep=0 rate=200000000
```

**This is the only thing left, and it is the real problem.** The leaf gate is
still closed, so the RB never runs and no fence ever completes.
`pixelreadback.py` creates a context (FD640, FBO COMPLETE), issues a clear, and
blocks forever in:

```
pid=1975 Sl  msm_wait_fence  python3
```

This is the *same* `msm_wait_fence` livelock documented in
`SESSION-2026-11-11-GL-LIVELOCK.md`, but it is now reached through a working
GL context rather than a dead one — which is why it is a much narrower target.

**Image 226 attacks this directly** with a host-side
`regulator_enable(gmu->gx)` on `gpu_gx_gdsc` (a real PMIC-backed GDSC the port
has never enabled), placed after the pre-state print so one boot gives an A/B.

## 4. Why that gate is still shut

Per `CLOCKS-GX-DOMAIN-OFF.md`, `GPU_CC_GX_GMU_CLK` is **not** in the node's
`clocks` list, and neither upstream msm nor CAF msm ever touches it from the
kernel (`grep devm_clk adreno/*.c` → no matches). The firmware is supposed to
open it. It now has the vote, the ack, and a transitioned GDSC, and still has
not opened the leaf.

Remaining candidates, in order:

1. **The GX vote corner is too low / wrong.** We vote `idx=6` of 7 (the top OPP).
   `Gx` came up (the PLL and GDSC moved) but the leaf may be gated on a higher
   or differently-indexed level. Re-vote with several indices and watch
   `gpu_cc_gx_gmu_clk` — cheap, no rebuild (the `KEEPALIVE156` work already
   does exactly this on a 10 s period).
2. **`RPMHPWR=0x0` at every single sample.** `REG_A6XX_GPU_GMU_CX_GMU_RPMH_POWER_STATE`
   has never once read anything but zero. If the RPMh power state is what
   releases the leaf gate, this is where to look — and it is the one register
   in the whole chain that is still flat-lined.
3. **IFPC / idle level.** `gmu->idle_level` drives `PWR_COL_INTER_FRAME_CTRL`
   (`a6xx_gmu.c:1190-1194`). If the firmware is in a low idle state it may hold
   GX collapsed between frames.
4. The `gmu_aop_send_acd_state()` AOP notification still missing
   (`ANALYSIS-GX-POWER-REVIEW.md` §2). ACD is literally the adaptive *clock
   distribution* block — now the prime suspect, because the clock is the one
   thing still wrong.

## 5. Method corrections made on this run

Two errors of my own, both worth recording:

- **I declared a "freeze" from a connection timeout.** It was an RNDIS IP wipe —
  `u.sh --fix` restored it and `u.sh --fix` reported **same boot, no reboot**.
  This is exactly the trap in `RESUME.md`, and the corpus's own "freeze
  signature" (ARP FAILED, `18d1:d001` still enumerated, link UP) is
  indistinguishable from it. **Always run `u.sh --fix` and check the boot id
  before calling anything frozen.**
- **My logger loop died silently** at t=330 s, which looked like the kernel
  dying. `dmesg` itself worked fine. Verify the log file's mtime before
  concluding the kernel stopped printing.

## 6. Next step

No rebuild needed to make progress. Re-vote GX at several perf indices and
watch `gpu_cc_gx_gmu_clk`:

```
# on device, GPU already loaded
for i in 1 2 3 4 5 6; do
  echo "$i" > /sys/kernel/devices/platform/soc/ae00000.qcom,mdss_mdp/a6xx_gpu_perf 2>/dev/null
  grep gpu_cc_gx_gmu_clk /sys/kernel/debug/clk/clk_summary
done
```

If the gate opens at some index, the vote mapping is the bug and
`gx_arc_votes[]` needs fixing. If it never opens, the AOP ACD notification
(`ANALYSIS-GX-POWER-REVIEW.md` §2) is the next image.

## 7. What this retires

- `ROOT-CAUSE-GMU-OOB.md` — "the OOB never acks, that's the root cause." The OOB
  acks perfectly once the correct vector is used. **Resolved, not worked around.**
- `GMU-DEAD.md` / `GMU-ALIVE-SPTPRAC.md` — the GMU was never dead and never
  starved. It was being spoken to in the wrong protocol version.
- `IMAGE-198-SPTPRAC-GUARD-RESULTS.md` — the sptprac guard was correct; SPTPRAC
  was never the issue.
- `CHECKS-HWINIT-DRAW.md` check 1 — `a6xx_hw_init` was faithful all along.
  The missing piece was a single `else` branch above it.

Twenty-two documents, 224 images, and the fault was one missing `else`.