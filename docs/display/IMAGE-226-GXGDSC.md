# IMAGE 226 — attacking the last gate: `gpu_cc_gx_gmu_clk` en=0

Date: 2026-10-12
Image: `artifacts/flash/226-halium-boot-a6xx-gxgdsc.img`
sha256: `9ffda201dcc86827ba527364757b76209dbc382d35c38453b9b739d05acb148d`
Parent: image 225 (which carried the `set_freq` + OOB vector fix)

| artifact | sha256 |
|---|---|
| `Image` | `fcc43b6cbef979cad6d36927b703bdb8d088c89080907e562c48f4a0ba731572` |
| `Image-dtb` | `02f392713b12413227496b1bb234ac0d6903d0634df148b008208e040af7866d` |

Pure kernel swap into image 225 — header, ramdisk and trailer byte-identical
(verified programmatically), cmdline unchanged.

---

## The problem

After image 225 the driver finally *asks* correctly, and the firmware answers:
GX vote acked 3×, `GPU_SET` OOB acked, `GX_GDSCR` transitioned
`0x222001 → 0xf8222800`, GPU PLL alive at 600 MHz, `RENDERER : FD640`.

And `gpu_cc_gx_gmu_clk` is **still `en=0`**. Everything above the leaf gate is
now verified correct; the leaf gate is the whole remaining fault.

## The experiment

**Host-enable the GX power domain.** `gpu_gx_gdsc` is real hardware, not a
placeholder:

```
sm8150-gdsc.dtsi:228   gpu_gx_gdsc: qcom,gdsc@0x2c9100c { sw-reset, domain-addr }
sm8150.dtsi:4121       status = "ok"; parent-supply = <&pm8150l_s2_level>
```

A genuine GDSC CSR behind a real PMIC rail. We already hold the handle
(`devm_regulator_get(gmu->dev, "vdd")`) and have **never** enabled it — resume
enables CX only. `IMAGE-83-84-RESULTS.md:58` proposed precisely this a hundred
images ago; `KGSL-DRM-POWER-COMPARE.md:58` later closed it on the claim that GX
comes up via GMU/RPMh alone. That claim is now testable, because the GMU/RPMh
path has actually been exercised and the gate is still shut.

Placed **after** the `GXVOTE225` pre-state print, so one boot yields an A/B.

Behind `a6xx.enable_gx_gdsc` (default **1**). Set `a6xx.enable_gx_gdsc=0` on the
cmdline to reproduce image-225 behaviour exactly.

### Reading the result

| outcome | conclusion |
|---|---|
| `GX_GDSCR` / `SPTPRAC` / `TCS0` move | the rail **is** host-reachable; the break is strictly GMU→AOP → build the AOP/QMP ACD notification |
| nothing moves | the host cannot reach it at all; AOP/QMP ACD notification is also the fix, and the GX GDSC must be voted via RPMh |
| `regulator_enable` returns non-zero | the regulator itself refused; read the error, that is the answer |

## Also in this image

1. **Fix my own bug — the poller now actually arms.** Image 225's `RPV225`
   emitted **zero** lines: the poller is gated on `experimental_nonlegacy` but
   the live cmdline sets `al_nonlegacy`, a different parameter name. The RPMh/
   TCS instrumentation added specifically to interpret image 225 never
   executed. Now armed unconditionally.
2. **Honest `a6xx_gmu_gx_is_on()`.** A reading of `SPTPRAC==0x0` is no longer
   treated as "GX on": `221-full.txt` shows `0x0` on a boot whose own
   `clk_summary` proves GX unclocked. The helper now returns false unless the
   SPTPRAC power FSM reports a settled state.
3. **`HOST2GMU_INTR_CLR` is now written** (0x5195), with before/after logging.
   That register was written nowhere in this tree, so OOB request bit 16 — set
   since image 1, never consumed — has been un-retractable for the life of the
   project.
4. **Balanced regulator disable.** `a6xx_gmu_reset()` now force-disables
   `gmu->gx` only if we enabled it. Image 148 already hit an
   unbalanced-disable panic on this exact path.
5. **devfreq-idle guard** in `a6xx_gmu_set_freq()`, matching upstream v5.8 —
   don't take power or block in the 5 s HFI waiter for an event that arrives
   while the GPU is idle.

## Deliberately not done

**The AOP/QMP ACD notification.** `CONFIG_MAILBOX=y` and
`include/linux/mailbox/qmp.h` are present, and `msm_qmp.c` drives the
`qmp_aop` node, so it is implementable. But CAF's exact ACD packet format could
not be sourced, and inventing a value is the single failure mode this project
has already documented twice (`CONSULT-MUSE-SPARK-CP-IRQ.md`: two consultants,
two confident wrong answers, both from inventing rather than reading). Image 226
runs the experiment that requires no invented protocol instead; if it comes back
"nothing moved", that is the next image.

## Test procedure

Unchanged: wait past the 300 s `gpu_init_delay_ms` deferral, start the on-device
dmesg logger **before** the trigger, trigger detached, and run `u.sh --fix` before
concluding anything from a connection timeout.

```
GXGDSC226: regulator_enable(vdd/gpu_gx_gdsc) ret=? already=?
GXGDSC226: +5ms SPTPRAC=0x.. GX_GDSCR=0x.. TCS0=0x.. RSC0=0x.. RPMHPWR=0x.. gx_is_on=?
OOBDIAG226: H2G_RAW=0x.. G2H_INFO=0x.. ...
GVIT226: poller armed
RPV225: TCS0=0x.. ... GX_GDSCR=0x.. RPMH_CTRL=0x..
```

Then `gpu_cc_gx_gmu_clk` in `clk_summary`, and the `pixelreadback.py` fence
test.