# Audit: the `cx_is_on` / cx-gdsc / SECVID family — all closed, by source, without a flash

Date: 2026-10-18
Trigger: image 275 (the `regulator_set_mode` probe) and step 3 of the KGSL diff.

## Verdict

**Every avenue in this family is a dead end, and each is now closed by source
reading rather than by guesswork.** The audit cost no device time.

## 1. `cx_is_on` would be INERT

KGSL's `a6xx_gmu_cx_is_on()` is called from exactly one place —
`gmu_disable_gdsc()` (`kgsl_gmu.c:1567,1573`):

```c
	ret = regulator_disable(gmu->cx_gdsc);
	...
	/*
	 * After GX GDSC is off, CX GDSC must be off
	 * Voting off alone from GPU driver cannot Guarantee CX GDSC off.
	 * Polling with 5s timeout to ensure
	 */
	do {
		if (!gmu_core_dev_cx_is_on(device))
			return 0;
		usleep_range(10, 100);
	} while (!(time_after(jiffies, t)));
```

Its entire purpose is to *poll that the CX GDSC actually powered down* after
`regulator_disable()`.

**This tree never calls `regulator_disable()` anywhere**, and has no GMU suspend
path at all (no `a6xx_gmu_suspend()`, no `.runtime_suspend` for the GMU device).
So there is no point at which a "is CX on" answer is needed, and implementing
`cx_is_on` would change nothing. The quirk's other half — the *mode* — was
tested in images 274 (NORMAL) and 275 (IDLE); **both abort the GPU's secure
firmware**, so the mode is a sensitive trigger, not a missing setting.

## 2. The KGSL-vs-DRM power/start divergences are all closed

`ANDROID-KGSL-PSP-REFERENCE.md` (written at image 62) listed three and said
"Next: single-variable test of divergence #1 (WFI_CONFIG) and/or #2 (HFI
units/count)". All are implemented in the tree:

| divergence | where it is now |
|---|---|
| `WFI_CONFIG = 0` before SYSRESET release | `a6xx_gmu.c:726`; constant `0x5002` in `a6xx_gmu.xml.h` |
| SYSRESET deasserted with no 1→0 pulse | `a6xx_gmu.c:766` |
| HFI 4-queue topology | `a6xx_gmu.h:167` (image 101) |
| AOP/ACD mailbox first | image 230, `ACD230: '{class: gpu, res: acd, value: 1}' ret=0` |
| DDR 300 MHz bus vote before FW_START | `a6xx_gmu.c:2139` |

## 3. The SECVID / TSB registers are NOT unprogrammed by omission

The live values on this device:

```
SECVID134: TRUST_CNTL=0x1  TSB_CNTL=0x0  TSB_BASE=0x0  SIZE=0x0  STATUS=0x1  mode=0
```

This tree's own **a5xx** driver puts the TSB registers in *exactly* that state
on purpose (`a5xx_gpu.c:531-539`):

```c
	gpu_write(gpu, REG_A5XX_RBBM_SECVID_TSB_CNTL, 0);
	/*
	 * Disable the trusted memory range - we don't actually supported secure
	 * memory rendering at this point in time and we don't want to block off
	 * part of the virtual memory space.
	 */
	gpu_write64(gpu, ...TSB_TRUSTED_BASE_LO..., 0x00000000);
	gpu_write(gpu, ...TSB_TRUSTED_SIZE, 0x00000000);
```

So a6xx is in the *intended* state, and the trusted window is deliberately
disabled. Also checked and closed:

- the `SECVID_TRUST_CNTL` write that the a6xx path skips (because
  `experimental_nonlegacy=1`) would be a **no-op**: the live value is already
  `0x1`.
- the a6xx `CP_PROTECT` list is **a6xx-specific and coherent**:
  `CP_PROTECT_CNTL = 0x3` with `CP_PROTECT(0..6)`, where
  `CP_PROTECT(6) = A6XX_PROTECT_RW(0xf000, 0x810)` covers the SECVID/TSB
  block at 0xf400/0xf800. The a5xx list (which includes a "UCHE" entry at
  0xE80) uses a different register layout and does **not** transfer.

## What this leaves

The whole "secure gate" family is closed:

- the *mode* — tested, aborts the HYP either way;
- `cx_is_on` — would be inert;
- the *register* protection list — coherent for a6xx;
- the trusted window — deliberately off.

**And the one thing this whole family has been circling is the thing that is
still unexplained:** the EL2/HYP data abort, which is **167 of the project's 213
archived captures** and happens on most boots *without the GPU being brought
up*. It is a fault in the GPU's *secure* firmware, so no CPU-side instrument in
this project can see it — the only handle is the bootloader's encrypted HYP log.

The one systematic comparison still available is
`ANDROID-KGSL-PSP-REFERENCE.md`'s working reference: *"GMU firmware boots with no
SMMU panic"* under KGSL. The a5xx/a6xx register programming is now provably
equivalent, so what remains is the **firmware's environment** — the memory
layout and the tables it reads at startup:

- the HFI table's **base address**: KGSL's is at `0x60100000` with a dummy page
  at `0x60000000` and a LOG at `0x60200000`; ours is the driver's own BO at
  whatever the drm_mm gives it, and the dummy/LOG slots are absent
  (divergence #3).
- the HFI table's **bounding size** in `HFI_QTBL_INFO`: ours writes `1`.
  The per-message header's SIZE field is in dwords and is correct
  (`a6xx_hfi.c:135` checks `HFI_HEADER_SIZE(hdr) > dwords`), but the *hardware's*
  view of the table length is a separate register.

**If the firmware is told the table is longer than it is, it walks off the end of
the table into the GMU's ITCM/dcache — an unmapped read at EL2 — which is
precisely the signature we keep seeing.** That is a concrete, testable,
single-variable hypothesis, and it needs no register archaeology: put the HFI
table where KGSL puts it (`0x60100000`), with the dummy page at `0x60000000` and
the LOG at `0x60200000`, and give `HFI_QTBL_INFO` the length the firmware
expects.

That is the recommended next experiment.
