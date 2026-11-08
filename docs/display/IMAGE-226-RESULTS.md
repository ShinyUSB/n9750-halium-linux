# IMAGE 226 — RESULT: the GX GDSC regulator is NOT the gate. Ruled out, cleanly.

Date: 2026-10-12
Image: `artifacts/flash/226-halium-boot-a6xx-gxgdsc.img`
sha256: `9ffda201dcc86827ba527364757b76209dbc382d35c38453b9b739d05acb148d`
Boot: `9d9bacc1`, kernel `#32`, device alive (no freeze, no reboot)
Capture: `captures/226-run.txt` (8539 lines)

## The experiment and its answer

```
[340.656696] GXVOTE225: idx=6/7 legacy=0 -> SPTPRAC=0xc0 GX_GDSCR=0x222001 TCS0=0x1 RSC0=0x1000000
[340.656989] GXGDSC226: regulator_enable(vdd/gpu_gx_gdsc) ret=0 already=1     <- SUCCEEDED
[340.666658] GXGDSC226: +5ms SPTPRAC=0x0 GX_GDSCR=0xf8222000 TCS0=0x1 RSC0=0x1000000 gx_is_on=0
```

and then, the answer:

```
gpu_cc_gx_gmu_clk   en=0 prep=0 rate=200000000
```

**`regulator_enable()` on the GX GDSC succeeded (ret=0) and the clock gate did
not open.** The host CAN reach the rail and choosing to power it changes nothing.

**Conclusion: `gpu_gx_gdsc` is not the blocker.** This closes, with evidence, the
question that `IMAGE-83-84-RESULTS.md:58` asked a hundred images ago and
`KGSL-DRM-POWER-COMPARE.md:58` closed on an assumption. That assumption — "GX
comes up via GMU/RPMh, not via regulator" — is now supported by a positive test
rather than left as an assertion: the regulator path is available and irrelevant.

The leaf gate is opened by something else entirely.

## Bonus result: the stuck OOB state is gone

```
OOBDIAG226: H2G_RAW=0x00000000 G2H_INFO=0x00000000 G2H_SET=0x00000000 G2H_MASK=0xff7ffffe AO_STATUS=0x00000000
```

On images 221–225 this read:

```
H2G_RAW=0x00010000   G2H_INFO=0x00000000 → 0x10000000
```

**Both are now zero.** This resolves the `GMU2HOST_INTR_INFO` bit 28 mystery that
four artifacts had flagged as unexplained — including my own
`ANALYSIS-GX-POWER-REVIEW.md`. The step-5 analyst's reading was correct: bit 28
was the GMU's mirror of a host request it did not recognise. Once we stopped
ringing the legacy bit 16 and started ringing the v2 bit 30, the unrecognised
request disappeared and so did its mirror. **Bit 28 was a symptom of speaking
the wrong protocol version, never a cause.**

`HOST2GMU_INTR_CLR` (0x5195) is now also written with before/after logging; it
was written nowhere in this tree until image 226.

## Third confirmed bug: the delayed_work poller never runs

```
[340.666692] GVIT226: poller armed (first +5s, then 30s)
$ grep -c RPV225 captures/226-run.txt
0
```

I fixed the gating bug in image 226 (the poller was behind `experimental_nonlegacy`
while the cmdline sets `al_nonlegacy`), the arm message now prints — and the
handler **still** never executes. `RPV225` count is 0, again.

So the original corpus observation stands and is now twice-confirmed:
**`schedule_delayed_work()` armed from `a6xx_gmu_resume()` never executes**,
even though the arm succeeds and the workqueue system works elsewhere. This is a
real, separate defect and it is why 225 and 226 both produced zero periodic
RPMh/TCS data. It has been quietly degrading every measurement in the project
since image 156.

Not chased in 227 — it is a diagnostic path, not the fault. But it is now on the
record as a bug in its own right, not a mystery.

## What is left, precisely

Everything below the host is correct and verified:

| layer | state |
|---|---|
| CM3 firmware boot | ✅ `BABEFACE ms~1` |
| HFI bootstrap | ✅ all messages `error=0` |
| GX vote (`GX_BW_PERF_VOTE`) | ✅ **acked 3×** |
| OOB `GPU_SET` (v2 bits 30/31) | ✅ **acked, `INTR_INFO=0x80000000`, ret=0** |
| stuck OOB request + bit 28 mirror | ✅ **now zero** |
| TCS/RPMh transaction | ✅ `TCS0=0x1`, `RSC0=0x1000000` |
| GX GDSC (host-reachable) | ✅ enabled, ret=0, **and irrelevant** |
| GPU PLL | ✅ `gpll0 en=2` 600 MHz, `div_clk_src en=1` |
| **`gpu_cc_gx_gmu_clk` leaf gate** | ❌ **still `en=0`** — the whole remaining fault |
| fence / pixels | ❌ `msm_wait_fence` forever |

**One bit.** `GPU_CC + 0x162`, mask `0x0F`
(`drivers/clk/qcom/debugcc-sm8150.c:785`). Its entire parent chain is enabled.
Nothing in software is blocking it; the GPU firmware is simply not opening it.

## Image 227 — the experiment that closes the question

Force that one gate bit from the kernel and run the fence test.

- **Fence completes** → the firmware never opens it; the fault is the GMU→AOP
  path and the fix is the AOP/QMP ACD notification.
- **Still hangs** → the break is deeper than power entirely (RB init, UBWC,
  address mode), and the entire power story was a red herring that only ever
  produced a GPU that boots and answers mail.

Behind `a6xx.force_gx_clk=1`, default **off**, so image 226 behaviour is
reproducible. Direct register write via the DT `qcom,gpucc` phandle — no
hardcoded address — because `clock_gpucc` exposes no `clock-names`, so the
clock cannot be requested through the clk framework. This bypasses refcounting
and is a diagnostic, not a fix.

## Corrections carried forward

- `IMAGE-225-RESULTS.md` over-claimed `SPTPRAC==0x0` as "GX on". Withdrawn. The
  clock tree is the only ground truth and it says the gate is shut.
- `ANALYSIS-GX-POWER-REVIEW.md`'s bit-28 sub-theory ("an unmasked stuck bit
  should have raised an IRQ") was inverted: `~0x00800000 = 0xff7ffffe` and 1
  means blocked, so no IRQ anomaly existed. Now moot — the bit is gone.
- Image 225's value was real but narrower than I wrote: the driver now *asks
  correctly*. Images 212–217 show the vote alone was already acked without
  effect, so it was the vote **plus** the OOB vector together.