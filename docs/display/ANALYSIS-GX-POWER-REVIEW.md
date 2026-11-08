# Review of the GX-power conclusion — three corrections, one untried fix, one new lead

Date: 2026-10-12
Scope: everything up to image 224 (`IMAGE-224-SPTPRAC.md`, `GMU-ALIVE-SPTPRAC.md`)
Sources: this directory's `.md` corpus, `captures/221-full.txt`,
`ShinyKernel-gpu-exp/Kernel/drivers/gpu/drm/msm/adreno/{a6xx_gmu.c,a6xx_gmu.h,a6xx_hfi.c}`,
`.opencode-tmp/donor/master/adreno/a6xx_gmu.c`, upstream commit
"drm/msm/a6xx: Send ACD state to QMP at GMU resume".

---

## 0. Verdict in one paragraph

The fault is genuinely below the driver, in the **GMU-firmware → RPMh → AOP**
link, exactly as `ROOT-CAUSE-GMU-OOB.md` concluded. But the two most recent
documents draw conclusions from that fact that are **not supported**, and both
point image 225 at targets that cannot work. The single highest-value step —
**sending the AOP/QMP ACD state**, which is named as missing in
`KGSL-DRM-POWER-COMPARE.md:46` and was never attempted in 224 images — remains
open and should be next. There is also one observation in the logs that **no
document mentions**: `GMU2HOST_INTR_INFO = 0x10000000` is set from the first
sample after bring-up and never cleared, and the GMU IRQ handler has **never run
once** in the entire project.

---

## 1. Corrections

### 1.1 "SPTPRAC is off, so the GMU firmware cannot run" is self-refuting

`GMU-ALIVE-SPTPRAC.md` builds its case on `SPTPRAC_PWR_CLK_STATUS = 0xc0` and
concludes SPTPRAC is powered down, therefore the GMU firmware cannot execute its
main loop.

The same document proves the opposite two paragraphs earlier:

```
HFI_VERSION=0x20000001          <- firmware wrote this
hfi poll ok (STATUS=0x1)        <- firmware is servicing the HFI queue
HFI-ACK id=1/4/3/11/14/10 error=0   <- six messages ACKed
```

Servicing an HFI queue and writing a version register **requires the firmware to
be executing**. The document's own unifying theory ("The GMU responds to plain
MMIO because that path needs no code execution") cannot be true, because the
HFI ACK path is the most code-execution-dependent thing in the whole design.

Upstream agrees, and says so explicitly. `donor/master/adreno/a6xx_gmu.c:487`:

```c
int a6xx_sptprac_enable(struct a6xx_gmu *gmu)
{
	WARN_ON(!gmu->legacy);
	/* Nothing to do if GMU does the power management */
	if (gmu->idle_level > GMU_IDLE_STATE_ACTIVE)
		return 0;
```

and `donor/master/adreno/a6xx_gmu.c:1049`:

```c
	if (gmu->legacy) {
		ret = a6xx_gmu_gfx_rail_on(gmu);
		...
		ret = a6xx_sptprac_enable(gmu);
```

**On HFI v2 (the A640, `HFI_VERSION=0x20000001`) upstream never touches SPTPRAC
at all and WARN_ONs if you try.** The host must not take SPTPRAC over. The
in-progress `SPTPRAC225` change in `a6xx_gmu.c:894` already reflects this and is
correct — do not revert it.

**Corollary that also kills the 0xc0 reading:** if SPTPRAC bits (0x01–0x10) read
*zero* — neither `POWER_ON` (0x04) nor `POWER_OFF` (0x08) nor `POWERING_ON`
(0x02) — then SPTPRAC is not in a host-observable off state at all. It is
firmware-managed. `IMAGE-224-SPTPRAC.md`'s decoding of `0xc0` as "GX_HM_GDSC_POWER_OFF
| GX_HM_CLK_OFF" is correct as a bit-decode; its inference that this means "SPTPRAC
is off" is not supported by the bits.

### 1.2 "The register did not latch, so the GDSC is not accepting power sequencing"

`IMAGE-224-SPTPRAC.md` writes 0x50d0, observes no change, and concludes "this is
the first direct evidence that the GDSC is not accepting host-driven power
sequencing at all".

`0x50d0` is `REG_A6XX_GMU_SPTPRAC_PWR_CLK_STATUS` — a **status** register, by
name and by upstream's usage. Upstream never writes it; it writes
`REG_A6XX_GMU_GX_SPTPRAC_POWER_CONTROL` (0x81). A read-only register not latching
a write is not evidence about the GDSC. The image-224 conclusion is a non-finding,
and it should not be cited as one.

### 1.3 `qcom,gpu-quirk-cx-gdsc` is a suspend-path quirk — it cannot explain this

`IMAGE-224-SPTPRAC.md` names "port KGSL's cx-gdsc workaround" as the required
next move. The project's own audit already described what that quirk does:

> `KGSL-DRM-POWER-COMPARE.md:65` — "CX handling (`regulator_set_mode IDLE/NORMAL`
> in **suspend**; GDSC polling) is gated on `ADRENO_QUIRK_CX_GDSC`"

It gates suspend-side regulator mode and disable ordering. The fault is that GX
never powers **on**. Nothing in that quirk can reach it. Do not spend image 225
here.

### 1.4 The GPU_SET OOB bit fix is right, but it is not the root cause

The `IMAGE 225: THIS IS THE BUG` comment in `a6xx_gmu.h:243-263` is **correct
and verified**. `donor/master/adreno/a6xx_gmu.c:356-365`:

```c
	[GMU_OOB_GPU_SET] = {
		.name = "GPU_SET",
		.set = 16,  .ack = 24,
		.set_new = 30, .ack_new = 31,
```

Legacy is HFI v1 only; the A640 runs v2.1, so 30/31 is right, and the
`HOST2GMU_RAW=0x00010000` observation (bit 16 sitting set, never consumed) is
correctly explained by ringing bit 16 at v2 firmware.

But be clear about what it buys. Upstream sends `GMU_OOB_GPU_SET` in exactly two
places — the IRQ handler (`master:1427`) and inside `gfx_rail_on()`, which is
itself legacy-only. Its documented purpose is *"let the GMU know to not turn off
any GPU registers while the CPU is in a critical section."* It is a
keep-alive hint. **Even a correctly acknowledged GPU_SET does not power GX.**
If image 225 shows the ack arriving and GX still off, that is the expected
result, not a surprise.

---

## 2. What the corpus already flagged and never ran

`KGSL-DRM-POWER-COMPARE.md:46` lists step 1 of KGSL's `gmu_start()`:

```
1. gmu_aop_send_acd_state()          AOP/QMP ACD mailbox
```

status **open**. A grep across all `.md` files for `aop_send`, `CMD_PKT`,
`qmp_send` returns only the three places that *identify* it as missing
(`IMAGE-76`, `IMAGE-77`, `KGSL-DRM-POWER-COMPARE`). **It has never been tried.**

It is not only a KGSL-ism. Upstream msm has the same thing:

> **drm/msm/a6xx: Send ACD state to QMP at GMU resume** (Konrad Dybcio, v5)
> "The QMP mailbox expects to be notified of the ACD (Adaptive Clock
> Distribution) state. Get a handle to the mailbox at probe time and poke it at
> GMU resume."
> ```c
> if (!IS_ERR(gmu->qmp))
>         qmp_send(gmu->qmp, "{class: gpu, res: acd, val: 0}", 0);
> ```

with `gmu->qmp = qmp_get(gmu->dev);` at probe. Our tree is v5.8-era, predating
it, and has no equivalent — `grep -n "qmp\|AOP" adreno/*.c` → nothing.

And the plumbing is already in the device tree, unused —
`sm8150-gpu.dtsi:420`:

```dts
		/* AOP mailbox for sending ACD enable and disable messages */
		mboxes = <&qmp_aop 0>;
		mbox-names = "aop";
```

`IMAGE-76-RESULTS.md:69` quotes this exact line and moves on.

**Caveat that must not be skipped:** upstream's `qmp_get()` + `"{class: gpu,
res: acd, val: %d}"` is the **AOSS** binding (SM8450+). This is SM8150, whose
`qmp_aop` is the CAF `qcom,qmp-mbox` rproc at 0xc300000 with a different packet
format. Port this from CAF's `qmp_aop` packet format, not from the upstream
string. Also, on SM8150 ACD may legitimately be a no-op (upstream hardcodes
"val: 0" everywhere precisely because it does not implement ACD) — so treat this
as *the* cheapest discriminating experiment against the RPMh/AOP gap, not as a
guaranteed fix. Risk is nil: it is an "off" notification.

---

## 3. New observations (not in any `.md`)

### 3.1 A GMU→host interrupt bit is stuck set, and the handler that would clear it has never run

From `captures/221-full.txt`, every `GVIT157` sample from 468 s to 2127 s:

```
GVIT157: SPTPRAC=0x0 RPMHPWR=0x0 GXVOTE=0x0 MXVOTE=0x0
         OOB_H2G=0x10000 OOB_G2H=0x10000000  hfi_irq=7 gmu_irq=0
```

Two things:

**(a) `OOB_G2H` is `0x10000000` (bit 28) at every single sample, from before the
first OOB request onward, and is never cleared.** It is also *unmasked*: the
`GMUDEAD` dump in `GMU-ALIVE-SPTPRAC.md` shows `OOB_G2H_MASK=0xff7ffffe`, which
has bit 28 set. It is the only stray bit in the register — and no document in
this directory has ever identified it. (It is plausibly the AOP ack in the CAF
bit layout, which would tie it to §2. CAF's public `a6xx_gmu.xml.h` defines only
`MSGQ` and `CM3_FAULT`, so **identify it before assuming** — dump it once and
resolve the constant against the KGSL private header.)

**(b) `gmu_irq=0` — `a6xx_gmu_irq()` has never executed once, in any image, ever.**
Meanwhile `hfi_irq=7`. `ROOT-CAUSE-GPU-IRQ-NEVER-FIRES.md` established the same
for the *GPU core* line (IRQ 487) and framed it as an independent problem. It is
not independent — it is the same missing-GX symptom seen from the interrupt side
— but the GMU line adds a new fact: **an unmasked, asserted bit in
`GMU2HOST_INTR_INFO` did not deliver an interrupt either**, and `/proc/interrupts`
for `kgsl_gmu_irq` (IRQ 305) has never been captured to cross-check.

And our handler cannot be clearing it, because **upstream's does and ours does
not** (`donor/master/adreno/a6xx_gmu.c:66-67`):

```c
	status = gmu_read(gmu, REG_A6XX_GMU_GMU2HOST_INTR_INFO);
	gmu_write(gmu, REG_A6XX_GMU_GMU2HOST_INTR_CLR, status);
```

`a6xx_gmu.c:77-115` (`a6xx_gmu_irq`) touches only
`REG_A6XX_GMU_AO_HOST_INTERRUPT_STATUS`. **`GMU2HOST_INTR_INFO` is never read or
cleared anywhere in our tree's IRQ path.**

> **CORRECTION (later the same day).** The "(bit 28 unmasked)" above is
> **wrong**, and with it the "an asserted unmasked bit failed to raise an IRQ"
> sub-theory. `a6xx_gmu_irq_enable()` writes
> `GMU2HOST_INTR_MASK = ~A6XX_HFI_IRQ_MASK = ~0x00800000 = 0xff7fffff`, then
> `a6xx_hfi_irq_enable()` clears MSGQ giving `0xff7ffffe` — which is *exactly*
> the observed value. In this register **1 = blocked**, so bit 28 is
> **masked**. A masked-and-asserted bit raising no interrupt is entirely
> self-consistent; there is no IRQ-path anomaly to explain. Credit: the
> silicon-level analyst on the fan-out.
>
> What survives: bit 28 is still unidentified, still asserted from the very
> first sample, and still never cleared by anything in the tree. It is worth
> one explicit `GMU2HOST_INTR_CLR` write plus a re-sample to see whether it
> re-asserts — but it is **not** load-bearing, and the real root cause found
> afterwards is in `IMAGE-225-GX-VOTE-ROOTCAUSE.md`.

**Working hypothesis, unproven:** if bit 28 is an AOP/rpmh request-ack, and the
host is expected to service-and-clear it, then not sending the ACD state (§2) and
not clearing the ack are two ends of the same handshake — and the deadlock is
firmware-waiting-for-host while host-never-asked. That would explain, from one
cause, three things that currently have three separate explanations: GX never
powers, OOB never acks, and no GMU interrupt ever arrives.

**Cheap decisive test (no new image needed if added to 225):** from the GVIT157
poller, also print `AO_HOST_INTERRUPT_STATUS`, `GMU2HOST_INTR_SET`, and attempt
`GMU2HOST_INTR_CLR = status`, then re-print. If the bit clears and behaviour
changes, the handshake was the stall. Also capture `/proc/interrupts | grep -i
gmu` to confirm the line's own counter.

### 3.2 `gpu_gx_gdsc` is real, PMIC-backed hardware, and the port never enables it

`arch/arm64/boot/dts/qcom/sm8150-gdsc.dtsi:228`:

```dts
	gpu_gx_gdsc: qcom,gdsc@0x2c9100c {
		compatible = "qcom,gdsc";
		reg = <0x2c9100c 0x4>;
		domain-addr = <&gpu_gx_domain_addr>;
		sw-reset = <&gpu_gx_sw_reset>;
		status = "disabled";
	};
```

enabled in `sm8150.dtsi:4121` with **`parent-supply = <&pm8150l_s2_level>`** — a
real PMIC rail, not a dummy.

The driver already holds the handle (`a6xx_gmu.c:3363`):
`gmu->gx = devm_regulator_get(gmu->dev, "vdd");` (`vdd-supply = <&gpu_gx_gdsc>`,
`sm8150-gpu.dtsi:178`). `a6xx_gmu_resume()` enables `gmu->cx` and **never
`gmu->gx`**. The only other reference to it is `a6xx_gmu.c:1541` —
`regulator_force_disable(gmu->gx)` in the reset path, guarded by image 148 after
the unbalanced-disable panic.

`IMAGE-83-84-RESULTS.md:58` proposed exactly this experiment:

> Add the GX-side vote to match KGSL ordering before clocks — either
> `regulator_enable(gmu->gx)` in resume (minimal) or the RPMh gfx-rail vote

It was never run. `KGSL-DRM-POWER-COMPARE.md:58` later closed it with the
opposite assertion ("GX comes up via GMU RPMh, not via regulator") — but that
assertion sits awkwardly with image 224's own reading of `GX_HM_GDSC_POWER_OFF`
and with `IMAGE-130-GXSTATE-RESULTS.md`, which recorded
`SPTPRAC=0x80 GDSC_OFF=0` at image 130 and `0xc0` (GDSC_OFF=1) at image 224.
**The GDSC-off bit changed between images and no document explains that.**

Honest weighting: upstream master says a host-side GX GDSC vote "won't actually
do anything but ... refcounting" on A6x/A7x (`master:1266-1278`), so this is
probably **not** the final fix. It is a **one-line, zero-risk discriminator** that
settles whether the GX rail is host-reachable at all, and it retires an open
question the corpus has been going back and forth on for 150 images.

---

## 4. Where the chain actually breaks (the model to work from)

```
host → GMU registers      OK   (QTBL/SFR/BOOT_CONFIG/CM3_CFG verified exact,
                                  image 89; RINGHW writes latch)
host → GMU firmware       OK   (HFI v2.1, 6 ACKs error=0, hfi_irq=7)
firmware → GX_VOTE_IDX    OK   (VOTE151 writes 0x9; readback 0 25 ms later
                                  => the firmware READ AND CLEARED it)
firmware → RPMh TCS vote  ?    never measured — RSCC_TCS0..3_DRV0_STATUS and
                                 RSCC/RSC_STATUS0_DRV0 are read NOWHERE in the
                                 224-image corpus (grep: zero hits)
RPMh → AOP → gpu_gx_gdsc  ?    the suspected break
AOP → gpu_cc_gx_gmu_clk   OFF  measured: en=0, gcc_gpu_gpll0_clk_src en=0
```

**The single biggest hole in the evidence is the third-from-bottom row.** The
RPMh TCS status registers are the direct observable for "did the GMU actually
issue its power vote to AOP", and they have never been dumped. `GMU-ALIVE-SPTPRAC.md`
even says "Our `rpmh_init` already sets `RPMH_CTRL_GFX_VOTE_ENABLE`" — setting the
enable bit is not evidence that a vote was ever *issued*.

Add to any next image, in the GVIT157 poller (no rebuild risk, already running):

```
REG_A6XX_RSCC_TCS0_DRV0_STATUS .. TCS3   (are there outstanding votes?)
REG_A6XX_GPU_RSCC_RSC_STATUS0_DRV0
REG_A6XX_RSCC_SEQ_BUSY_DRV0
REG_A6XX_GPU_CC_GX_GDSCR                  (read from gmu->mmio)
```

`a6xx_gmu_rpmh_off()` (`a6xx_gmu.c:1523-1531`) already polls TCS0..3 — those
polling loops run on the force-off path and have never been instrumented.

---

## 5. Recommended order

| # | step | why now |
|---|---|---|
| 1 | Send AOP/QMP ACD state at GMU resume (CAF `qmp_aop` format, `val: 0`) | named open since `KGSL-DRM-POWER-COMPARE.md:46`, present in **both** reference implementations, zero risk |
| 2 | Dump `RSCC_TCS0..3_DRV0_STATUS` + `RSC_STATUS0_DRV0` + `GX_GDSCR` from the existing poller | the only direct observable for "did the GMU vote?"; zero new hardware behaviour |
| 3 | Read + clear `GMU2HOST_INTR_INFO` from the poller; capture `/proc/interrupts \| grep -i gmu` | bit 28 has been set and ignored since image 221; §3.1 |
| 4 | `regulator_enable(gmu->gx)` in `a6xx_gmu_resume()` before clocks | one line; settles the GDSC question the corpus has oscillated on for 150 images; `IMAGE-83-84` proposal |
| 5 | Only then revisit OOB GPU_SET ack and `gpu_gdsc` quirk parsing | #1 and #4 are strictly upstream of those |

**Keep:** the image-225 OOB bit fix (verified correct against
`donor/master:356-390`) and the image-225 `sptprac_enable` legacy guard (verified
correct against `donor/master:487,1049`). Both are right; neither is the bug.

**Do not:** chase `qcom,gpu-quirk-cx-gdsc` (§1.3), or re-run `a6xx_hw_init`
diffs (`CHECKS-HWINIT-DRAW.md` check 1 closed that).

---

## 6. Corrections to the record

- `GMU-ALIVE-SPTPRAC.md` — "SPTPRAC is not powered, therefore the GMU cannot run
  its main loop": **wrong**, refuted by its own HFI ACK evidence and by upstream
  gating `a6xx_sptprac_enable()` behind `gmu->legacy` + `WARN_ON(!gmu->legacy)`.
- `IMAGE-224-SPTPRAC.md` — "the register does not latch our write, therefore the
  GDSC rejects host-driven power sequencing": **non-finding**; 0x50d0 is a status
  register upstream never writes.
- `IMAGE-224-SPTPRAC.md` next-move 1 — port KGSL's `cx-gdsc` workaround:
  **wrong target**; it is a suspend-path quirk per the corpus's own audit.
- `KGSL-DRM-POWER-COMPARE.md:58` — "GX comes up via GMU RPMh, not via regulator":
  **unverified and load-bearing**; contradicted by the GX GDSC-off bit changing
  between images 130 and 224. Reopen as step 4.