# IMAGE 225 — ROOT CAUSE FOUND: non-legacy `set_freq` never sent the HFI GX vote

Date: 2026-10-12
Found by: cross-model fan-out (5 analysts) + direct diff against `donor/v58`, `donor/master`
Status: **fixed, built, packaged — awaiting flash**

## The image

```
artifacts/flash/225-halium-boot-a6xx-gxvote.img
sha256 1e8ddabd68f08a72527e03af4c83444dbb05a187cc3b771be96fbda65e9cb0f7
```

Built via `/home/usb/.opencode-tmp/build-gpu.sh` (Snapdragon clang,
`CROSS_COMPILE=toolchains/gcc-aarch64/bin/aarch64-linux-android-`).
Compiles clean — only a pre-existing unrelated `unused variable 'i'`
warning at `a6xx_gmu.c:2642`.

| kernel | sha256 |
|---|---|
| `Image` | `097af5c6aef6f6a3872cb44d2dbb4492c338190d1845646a24758f7041a281a4` |
| `Image-dtb` | `6f9001a4c4715c44535c88d53a303dc0e500622d0144d356377a79dd6be62d93` |

Packaged as a **pure kernel swap** into image 224: new `Image-dtb` is exactly
48418016 bytes, the same size as 224's kernel payload, so header, ramdisk and
trailer are byte-identical to 224 — verified programmatically. The kernel
cmdline is unchanged from 224, so this is a clean A/B against the last
known-good-flashing image.

## Test procedure (from `IMAGE-221-CLOCKS.md` §0 — do not skip)

`a6xx.gpu_init_delay_ms=300000`, so **wait until uptime > 300 s** before
triggering, or the device freezes on the deferred-init race. Start the
background dmesg logger *before* the trigger:

```
while true; do dmesg > /root/dlog225.txt; sleep 2; done
```

Then trigger `echo 1 > /sys/devices/platform/soc/ae00000.qcom,mdss_mdp/a6xx_gpu_load`
**detached**, and never let an ssh command span the trigger.

### What to look for, in order

```
GXVOTE225: idx=6/7 legacy=0 -> SPTPRAC=0x.. GX_GDSCR=0x.. TCS0=0x.. RSC0=0x.. RPMHPWR=0x..
RPV225: TCS0=0x.. TCS1=0x.. TCS2=0x.. TCS3=0x.. SEQ_BUSY=0x.. RSC0=0x.. GX_GDSCR=0x.. RPMH_CTRL=0x..
```

and then `gpu_cc_gx_gmu_clk` in `clk_summary`, and the `BOS[]` render-target
dump from image 221.

Note the `POST_START: oob GPU_DCVS ack-wait TIMEOUT` line should now be
**absent**, because `a6xx_gmu_set_freq()` returns before reaching the DCVS/OOB
code on non-legacy. If it still appears, the branch is not being taken.

## The bug, in one sentence

Our port had only the HFI **v1** frequency path and called it unconditionally.
On an A640 (`gmu->legacy == false`, HFI v2.1) that means the driver **never told
the GMU firmware which GX corner to vote for** — and `HFI_H2F_MSG_GX_BW_PERF_VOTE`
is the only message that makes v2 firmware raise the GX rail and
`gpu_cc_gx_gmu_clk`. Holding GX collapsed was, from the firmware's point of
view, the correct response to never being asked.

## The evidence (mechanical, not inferred)

`donor/v58/adreno/a6xx_gmu.c` has **two** implementations and branches on legacy:

```c
106: static void __a6xx_gmu_set_freq(struct a6xx_gmu *gmu, int index)   /* v1: DCVS regs + OOB */
139: void a6xx_gmu_set_freq(struct msm_gpu *gpu, unsigned long freq)
...
163:	if (gmu->legacy)
164:		__a6xx_gmu_set_freq(gmu, perf_index);
165:	else
166:		a6xx_hfi_set_freq(gmu, perf_index);
```

and again at the tail of `a6xx_gmu_resume()`:

```c
901:	if (gmu->legacy)
902:		__a6xx_gmu_set_freq(gmu, gmu->current_perf_index);
903:	else
904:		a6xx_hfi_set_freq(gmu, gmu->current_perf_index);
```

`donor/v58/adreno/a6xx_hfi.c`:

```c
int a6xx_hfi_set_freq(struct a6xx_gmu *gmu, int index)
{
	struct a6xx_hfi_gx_bw_perf_vote_cmd msg = { 0 };

	msg.ack_type = 1;	/* blocking */
	msg.freq = index;
	msg.bw = 0;		/* TODO: bus scaling */

	return a6xx_hfi_send_msg(gmu, HFI_H2F_MSG_GX_BW_PERF_VOTE, &msg,
		sizeof(msg), NULL, 0);
}
```

`donor/master/adreno/a6xx_gmu.c:221-222` keeps the same branch:

```c
	if (!gmu->legacy) {
		a6xx_hfi_set_freq(gmu, perf_index, bw_index);
```

**Our tree had neither the branch nor `a6xx_hfi_set_freq()`.** `grep -n
a6xx_hfi_set_freq` over our `adreno/` → zero hits. The only GX vote sender,
`a6xx_hfi_send_gx_vote()`, was labelled `/* TEMPORARY experiment image 132 */`
and reachable only from opt-in diagnostics (module param default off) plus a
zero-vote on suspend. **It was never once sent with a real operating point on a
normal boot.**

### Why every previous conclusion is consistent with this

| observation | explanation under this bug |
|---|---|
| GMU alive, HFI v2.1, 6 ACKs `error=0` | firmware is healthy; HFI transport works |
| CX domain fine, CP executes, ring drains | CX never depends on a GX vote |
| `gpu_cc_gx_gmu_clk en=0`, `gcc_gpu_gpll0_clk_src en=0` | GX rail never voted |
| `SPTPRAC_PWR_CLK_STATUS`=0xc0 (`GX_HM_GDSC_POWER_OFF|GX_HM_CLK_OFF`) | firmware is truthfully reporting an unvoted rail |
| `GX_VOTE_IDX` written 0x9, reads 0 later | the DTCM mailbox is not the v2 path at all — upstream never writes it on v2 |
| `POST_START: oob GPU_DCVS ack-wait TIMEOUT` | **same bug, other side**: DCVS OOB request bit 23 is undefined on v2 firmware, so it is never acked. Not an independent fault. |
| render target byte-for-byte zero | nothing in GX ever ran |

This retires the two theories in `GMU-ALIVE-SPTPRAC.md` and `ROOT-CAUSE-GMU-OOB.md`
at once: the OOB timeout was never a firmware handshake failure, it was us
speaking v1 to a v2 processor.

## The three changes made

### 1. `a6xx_gmu_set_freq()` branches on legacy — `a6xx_gmu.c`

```c
	if (!gmu->legacy)
		return a6xx_hfi_set_freq(gmu, index);
```

### 2. `a6xx_hfi_set_freq()` added — `a6xx_hfi.c`, declared in `a6xx_gmu.h`

Byte-identical to upstream v5.8. `struct a6xx_hfi_msg_gx_bw_perf_vote`
(`{header, ack_type, freq, bw}`) already matched upstream's
`a6xx_hfi_gx_bw_perf_vote_cmd` exactly, so no struct change was needed.

### 3. DCVS packing corrected — `a6xx_gmu.c`

```c
-	((index << 24) & 0xff) | (3 & 0xf));
+	((3 & 0xf) << 28) | index);
```

The `0xff` mask discarded every bit at or above 8, so the perf index could never
reach the register **even on legacy hardware**. Real bug, dead on A640.

### 4. Cold boot forced for non-legacy — `a6xx_gmu.c`

Restored `donor/v58:877-881`:

```c
	/* Warm boot path does not work on newer GPUs
	 * Presumably this is because icache/dcache regions must be restored */
	if (!gmu->legacy)
		status = GMU_COLD_BOOT;
```

Our port trusted `GENERAL_7` alone — but our own cold `fw_start` sets
`GENERAL_7=1`, so **every resume after the first selected WARM and skipped the
firmware reload**. A second, independent way GX never comes up.

## Diagnostics added (never dumped in 224 images)

`RPV225` in the existing 30 s poller — `TCS0..3_DRV0_STATUS`, `SEQ_BUSY_DRV0`,
`RSC_STATUS0_DRV0`, `GX_GDSCR`, `RPMH_CTRL`.

Every earlier document cited "`RPMH_CTRL_GFX_VOTE_ENABLE` is set" as evidence the
GMU votes GX up. **That is the vote *interface*, not a vote *transaction*.** These
are the registers that actually witness one, and they had never been read:

```
RPV225: TCS0=0x.. TCS1=0x.. TCS2=0x.. TCS3=0x.. SEQ_BUSY=0x.. RSC0=0x.. GX_GDSCR=0x.. RPMH_CTRL=0x..
```

- TCS bits still 0 and RSC0 static after the vote → firmware never issued one;
  the break is below the GMU.
- TCS moves → the vote went out; the break is AOP-side.

`GXVOTE225` reads SPTPRAC / GX_GDSCR / TCS0 / RSC0 / RPMHPWR immediately after
the vote — the first point in the project where we can see whether it had any
effect.

## Reading the result

| outcome | conclusion |
|---|---|
| **GX clocks on, render target written** | root cause confirmed and closed |
| vote ACK'd, GX still dark, TCS moved | vote issued; break is AOP/RPMh → pursue `gmu_aop_send_acd_state` (see `ANALYSIS-GX-POWER-REVIEW.md` §2) |
| vote ACK'd, GX still dark, TCS static | firmware ignored the vote → suspect `gx_arc_votes[]` index / PERF table contents |
| freeze on trigger | expected risk from 224 images; retry past the 300 s `gpu_init_delay_ms` deferral (see `IMAGE-221-CLOCKS.md` §0) |

## Corrections this makes to earlier documents

- `ROOT-CAUSE-GMU-OOB.md` — "the OOB handshake times out; that is why GX never
  powers." **Wrong.** The OOB timeout is a symptom of speaking HFI v1 DCVS/OOB
  to v2 firmware. It is not a firmware handshake failure.
- `GMU-ALIVE-SPTPRAC.md` — "SPTPRAC off ⇒ firmware cannot run its loop."
  **Wrong**, refuted by its own HFI ACK evidence; upstream has
  `WARN_ON(!gmu->legacy)` in `a6xx_sptprac_enable` and calls it only under
  `if (gmu->legacy)`.
- `IMAGE-224-SPTPRAC.md` — "0x50d0 did not latch our write, so the GDSC rejects
  host power sequencing." **Non-finding**: 0x50d0 is a status register upstream
  never writes.
- `IMAGE-130-GXSTATE-RESULTS.md` blamed the `(val & 0x38) == 0x28` poll. Upstream
  has that exact condition at `master:496`; it is correct, and it is unreachable
  on v2 anyway because upstream skips `sptprac_enable` entirely.

## Note on the fan-out

Five analysts on five model families, briefed independently and forbidden to see
each other. Two of the frontier models were unavailable (out of funds); the free
roster was used instead. **The finding came from the mechanical-diff analyst**
and was then verified line-by-line by hand against the donors before a single
line was changed — a claim like this is worth nothing until it has been diffed
against the real tree.

One correction to `ANALYSIS-GX-POWER-REVIEW.md` (mine, earlier today): I wrote
that `GMU2HOST_INTR_INFO` bit 28 was *unmasked* and that its being stuck set
should therefore have raised an interrupt. **That is wrong** — `a6xx_gmu_irq_enable()`
writes `~A6XX_HFI_IRQ_MASK` = `0xff7ffffe`, matching the observed value exactly,
and 1 means *blocked*. Bit 28 is masked, so a masked-and-asserted bit raising no
IRQ is entirely self-consistent, and the "the IRQ path is broken" sub-theory
collapses. The bit is still unidentified and still never cleared, so it remains
worth one readback — but it is not the smoking gun.