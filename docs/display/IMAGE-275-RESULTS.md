# Image 275 — IDLE kills it too: the cx-gdsc regulator-mode hypothesis is dead

Date: 2026-10-18
Image: `artifacts/flash/275-halium-boot-a6xx-cxidle.img`
sha256 `bd148cf2374b80a753b12d6978ff2857ffe2b2bda0cc902eda1fcb3aca8176b4`

## What the run showed

The dmesg was streamed to the host over SSH, so the last milliseconds survived
(`artifacts/gpu-bringup/evidence-274c/attempt-1/dmesg-stream.txt`, 5991 lines):

```
[  703.133424] A6XX_TRACE: before CX GDSC enable (cx=ffffffc368e6da00 already=0)
[  703.133461] A6XX_TRACE: after CX GDSC enable (ret=0 already=1)
[  703.133476] CXMODE274: cx=ffffffc368e6da00 gpu_node=ffffffc37fef0c48 quirk(cx-gdsc)=1 mode=4
[  703.133492] CXMODE274: before: enabled=1 mode=2
[  703.133505] CXMODE274: regulator_set_mode(vddcx, 4) = 0
[  703.133515] CXMODE274: after: enabled=1 mode=4
<stream ends>
```

The device reset within ~5 s of the GPU bring-up, at exactly the mode set.

Three results, all clean:

1. **Step 1 verified.** `quirk(cx-gdsc)=1` — the quirk is now read from the
   right node (image 274 read it from the GMU node and got 0).
2. **The mode is genuinely settable, and the readback is honest.** `before:
   mode=2` → `after: mode=4`. (In image 274 the mode read 2 both before and
   after, because NORMAL was requested and NORMAL was already the value — so
   that run proved nothing about the register.)
3. **IDLE kills it exactly like NORMAL did.** Both modes abort the GPU's secure
   firmware (the same `TZBSP_ERR_FATAL_NOC_ERROR` / EL2 data abort that
   accounts for 167 of the project's 213 archived captures).

## Conclusion

**The crash is not about *which* mode — it is about the act of changing the
mode while the GPU is up.** That is exactly why KGSL only does it in
`gmu_suspend()`, bracketed by a GDSC disable/enable:

```c
	gmu_disable_clks(device);
	if (QUIRK_CX_GDSC) regulator_set_mode(cx_gdsc, REGULATOR_MODE_IDLE);   /* before disabling */
	gmu_disable_gdsc(device);
	if (QUIRK_CX_GDSC) regulator_set_mode(cx_gdsc, REGULATOR_MODE_NORMAL);  /* after re-enabling */
```

**So the cx-gdsc regulator-mode hypothesis is dead.** The CX GDSC's mode is not
what the GPU is missing, and our tree's refusal to set it is correct on the
resume path.

## A structural fact that fell out of this

**Our tree has no GMU suspend path at all, and `regulator_disable()` is never
called anywhere in it.**

- no `a6xx_gmu_suspend()`,
- no `.runtime_suspend` for the GMU platform device,
- zero `regulator_disable()` calls in `adreno/`.

KGSL's IDLE→disable→NORMAL sequence exists because KGSL *does* power the CX
GDSC down on suspend. This driver brings it up at resume and never takes it
down. So the mode is not merely unset — **the whole power-down half of KGSL's
sequence is absent here.**

That is a real difference, and it points at the idle/power-state logic rather
than at a register write.

## Where that leaves the investigation

The substantive half of the cx-gdsc quirk is not the mode — it is
`a6xx_gmu_cx_is_on()`:

```c
	if (ADRENO_QUIRK(..., ADRENO_QUIRK_CX_GDSC))
		return regulator_is_enabled(KGSL_GMU_DEVICE(device)->cx_gdsc);
	gmu_core_regread(device, A6XX_GPU_CC_CX_GDSCR, &val);
	return (val & BIT(31));
```

It answers *"is the CX domain on?"* **from the regulator, not the register**,
because the register is unreliable on this part — the project's own image-226
note already established that for its sibling:

> IMAGE 226: a reading of 0x0 is NOT proof of life … it is what the register
> reads when nobody is driving the FSM. Treat it as UNKNOWN.

**This tree has no `cx_is_on` at all.** It has `a6xx_gmu_gx_is_on()`, documented
as ambiguous for exactly that reason, and it feeds the idle decision at
`a6xx_gmu.c:371`.

### The next step, in order

1. **Find our tree's equivalent of KGSL's `cx_is_on` call sites** and determine
   what the CX-on answer currently drives. In KGSL it is a `gmu_dev_ops`
   function pointer consulted by the idle/power-state machinery. In this tree the
   candidates are the runtime-PM transitions (`a6xx_gmu.c:287` `pm_runtime_suspended`,
   `:371` `a6xx_gmu_gx_is_on`, `:1711/1714` put/get) and the `a6xx_gmu.c:2703`
   `pm_runtime_put_sync`.
   **If the answer is currently unused, implementing `cx_is_on` would be inert** —
   so this audit has to come first, and it is free (source reading).
2. Only then build the `cx_is_on` change (regulator-based, quirk-gated).
3. Separately: the EL2/HYP data abort remains the project's dominant crash
   (167/213) and is still uninstrumented. It is a fault in the *secure* world,
   so no CPU-side instrument in this project can see it; the only handle the
   project has is the bootloader's encrypted HYP log, and
   `ANDROID-KGSL-PSP-REFERENCE.md` records a working KGSL boot where "GMU
   firmware boots with no SMMU panic". That comparison remains the highest-value
   one available.

## Images 274/275 summary

| image | change | outcome |
|---|---|---|
| 274c | `regulator_set_mode(vddcx, NORMAL)`; quirk read from the **GMU** node (wrong) | device reset immediately at the mode set; quirk reported 0 |
| 275 | `regulator_set_mode(vddcx, IDLE)`; quirk read from the **GPU** node (right) | device reset immediately at the mode set; quirk reported 1; mode confirmed to change 2→4 |
