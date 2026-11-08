# Steps 1–3: the cx-gdsc work, and where the KGSL diff actually points

Date: 2026-10-18
Images: 274c (NORMAL, crashed the device), 275 (IDLE + the quirk read fixed)

## Step 1 — the quirk read (fixed in image 275)

Image 274 read `qcom,gpu-quirk-cx-gdsc` from `gmu->dev->of_node` — the **GMU**
node. The property is on the **GPU** node:

```
sm8150-gpu.dtsi:116      qcom,gpu-quirk-cx-gdsc;         <- qcom,kgsl-3d0  (GPU)
sm8150-gpu.dtsi:175      regulator-names = "vddcx", "vdd";
sm8150-gpu.dtsi:403      regulator-names = "vddcx", "vdd"; <- qcom,gpu-gmu   (GMU)
```

Image 275 reads it from the right node, via
`a6xx_gpu->base.base.dev->dev_private->gpu_pdev->dev.of_node`, and logs it
(`CXMODE274: ... gpu_node=%px quirk(cx-gdsc)=%d mode=%u`). It should now report
**1**.

## Step 2 — the mode: the question was wrong, and that is itself a result

KGSL's `regulator_set_mode()` calls are in **`gmu_suspend()`**, and they bracket a
GDSC disable/enable:

```c
	gmu_disable_clks(device);
	if (QUIRK_CX_GDSC) regulator_set_mode(gmu->cx_gdsc, REGULATOR_MODE_IDLE);   /* before disabling */
	gmu_disable_gdsc(device);
	if (QUIRK_CX_GDSC) regulator_set_mode(gmu->cx_gdsc, REGULATOR_MODE_NORMAL);  /* after re-enabling */
```

**KGSL never sets that mode on the resume path.** So:

- our tree's behaviour (never setting it) is **correct** for bring-up;
- **image 274's experiment was invalid** — it poked the mode under a live GPU,
  which is an out-of-sequence state change, and the GPU's secure firmware
  (HYP) aborted on it. That is the *expected* consequence, and it makes 274's
  crash a **positive datum about the HYP's sensitivity**, not a null result;
- image 275 therefore keeps `a6xx.cxgdm=4` (IDLE) deliberately, as a
  **fault-injection probe**: if IDLE also aborts, the GDSC mode is a sensitive
  trigger for the HYP abort; if IDLE does *not* abort, the abort is specific to
  NORMAL, which would say something quite different (NORMAL being what the GPU
  wants, with something else wrong).

## Step 3 — the KGSL diff: every divergence the PSP doc lists is already closed

`ANDROID-KGSL-PSP-REFERENCE.md` (written at image 62) listed three KGSL-vs-DRM
divergences and said "Next: single-variable test of divergence #1 (WFI_CONFIG)
and/or #2 (HFI units/count), one at a time."

All of them are **already implemented** in the tree:

| PSP-doc divergence | state in the tree |
|---|---|
| #1 `WFI_CONFIG = 0` before SYSRESET release | **done** — `a6xx_gmu.c:726` `gmu_write(gmu, REG_A6XX_GMU_CX_GMU_WFI_CONFIG, 0x0)`; the constant `0x5002` is in `a6xx_gmu.xml.h` |
| SYSRESET: KGSL deasserts with no 1→0 pulse | **done** — `a6xx_gmu.c:766` `gmu_write(gmu, REG_A6XX_GMU_CM3_SYSRESET, 0)` |
| #2 HFI: 4 queues, size in dwords | **done** — `a6xx_gmu.h:167` `struct a6xx_hfi_queue queues[4]; /* TEMPORARY img101: vendor 4-queue topology */` |
| AOP/ACD mailbox first | **done** — image 230, `ACD230: '{class: gpu, res: acd, value: 1}' ret=0` |
| DDR 300 MHz bus vote before FW_START | **done** — `a6xx_gmu.c:2139` `msm_bus_scale_client_update_request(gmu->bw_pcl, 4)` |

## What the diff actually points at now

Not the mode. **The other half of the quirk.**

KGSL's `a6xx_gmu_cx_is_on()`:

```c
static bool a6xx_gmu_cx_is_on(struct kgsl_device *device)
{
	unsigned int val;
	if (ADRENO_QUIRK(ADRENO_DEVICE(device), ADRENO_QUIRK_CX_GDSC))
		return regulator_is_enabled(KGSL_GMU_DEVICE(device)->cx_gdsc);
	gmu_core_regread(device, A6XX_GPU_CC_CX_GDSCR, &val);
	return (val & BIT(31));
}
```

It is a **function pointer** in `gmu_dev_ops` (`.cx_is_on = a6xx_gmu_cx_is_on`),
and it exists because the `CX_GDSCR` register is **unreliable on this part** —
which the project's own image-226 note already established for its sibling:

> IMAGE 226: a reading of 0x0 is NOT proof of life … it is what the register reads
> when nobody is driving the FSM. Treat it as UNKNOWN.

**Our tree has no `cx_is_on` at all** — only `a6xx_gmu_gx_is_on()`, which is
documented as ambiguous for exactly that reason, and which is used in the
GMU idle path at `a6xx_gmu.c:371`.

So the substantive remaining gap in the cx-gdsc quirk is:

> **Answer "is the CX domain on?" from the regulator, not from the register, and
> use that answer in the GMU idle/power-state logic.**

That is where a wrong "the GPU is off" answer would cause a missed power step
and a hung GPU — and it is a much better target than a regulator mode poke.

**Caveat, stated honestly:** KGSL's `cx_is_on` is used by KGSL's idle-detection
machinery, and this tree's idle logic is a re-implementation. Whether our tree
*has* a place where the CX-on answer changes behaviour is not yet established —
that has to be checked before building it, otherwise the change would be inert.
The first job is therefore to find our tree's equivalent of KGSL's
`cx_is_on` call sites (`gmu_sptprac_is_on`, the idle-level decision at
`a6xx_gmu.c:371`) and determine what the answer is currently based on.

## Artifacts

```
274c  bd148... no: 8a68f8f442ecf0da3b094cce10d9682becf7411ed99936ee2a6329802f61ef0f  (NORMAL -- crashed the device)
275   bd148cf2374b80a753b12d6978ff2857ffe2b2bda0cc902eda1fcb3aca8176b4
      kernel ba24ef8b5f2379c5767fdb46a6f7f4d9760ad6385d56e891372f67eba92a4ba4
      cmdline  a6xx.cxgdm=4 (IDLE), quirk read from the GPU node
```

## Note on image 275

Image 275 still performs the (KGSL-invalid) mode poke, because it is now a
**fault-injection probe** rather than a fix attempt. If that reading is wrong —
if the mode poke is simply invalid and the only thing worth doing is the
`cx_is_on` work — then image 275 should be skipped and the next image should
carry only the `cx_is_on` change.
