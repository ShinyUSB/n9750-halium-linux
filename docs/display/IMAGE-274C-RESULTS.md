# Image 274c — the experiment ran, and the GPU's secure firmware aborted on it

Date: 2026-10-18
Image: `artifacts/flash/274c-halium-boot-a6xx-cxgdsc.img`
sha256 `8a68f8f442ecf0da3b094cce10d9682becf7411ed99936ee2a6329802f61ef0f`

## 1. The experiment executed — and the device died on it

The dmesg was **streamed to the host** over SSH (the phone's filesystem rolls back
on reset, and the RNDIS link is down exactly when it matters), so the last
milliseconds survived. From
`artifacts/gpu-bringup/evidence-274c/dmesg-stream.txt`:

```
[  369.204418] A6XX_TRACE: before CX GDSC enable (cx=ffffffc36f489880 already=0)
[  369.204456] A6XX_TRACE: after CX GDSC enable (ret=0 already=1)
[  369.204468] CXMODE274: cx=ffffffc36f489880 quirk(cx-gdsc)=0
[  369.204484] CXMODE274: before: enabled=1 mode=2
[  369.204494] CXMODE274: regulator_set_mode(vddcx, NORMAL) = 0
[  369.204505] CXMODE274: after: enabled=1 mode=2
<stream ends>
```

The device reset within milliseconds of
`regulator_set_mode(vddcx, REGULATOR_MODE_NORMAL)` — the same
`TZBSP_ERR_FATAL_NOC_ERROR` (EL2/HYP data abort) that accounts for **167 of the
project's 213 archived captures**. The `GXGDSC226` and `GXCLK229` blocks that
follow in the code never ran.

**So: doing the one thing KGSL does that this driver does not —
`regulator_set_mode()` on the GPU's CX GDSC — reproducibly kills the GPU's
secure firmware.** That is a real result, not a null one.

Note `before: enabled=1 mode=2` and `after: enabled=1 mode=2`:
`REGULATOR_MODE_NORMAL` is 0x2, so the *readable* mode was already NORMAL and
did not change. `regulator_get_mode()` on this rail evidently does not reflect
what the PMIC actually does, so "the mode did not change" cannot be concluded
from the readback — the call still did something at the PMIC, and the GPU's
secure side faulted on it.

## 2. A bug in my own patch: the quirk was read from the wrong node

```
CXMODE274: quirk(cx-gdsc)=0
```

but our DT **does** set it. The property lives on the **GPU** node, not the GMU
node:

```
sm8150-gpu.dtsi:115      qcom,gpu-quirk-secvid-set-once;
sm8150-gpu.dtsi:116      qcom,gpu-quirk-cx-gdsc;      <- the GPU node (qcom,kgsl-3d0)
sm8150-gpu.dtsi:175      regulator-names = "vddcx", "vdd";
sm8150-gpu.dtsi:403      regulator-names = "vddcx", "vdd";   <- the GMU node (qcom,gpu-gmu)
```

My code read `gmu->dev->of_node`, which is the **GMU** node
(`2c6a000.qcom,gmu`) — so the quirk read 0. KGSL reads it from the *adreno
device* node (`2c00000.qcom,kgsl-3d0`), where it is set.

**Fix:** read the quirk from the GPU node. The driver can reach it via
`priv->gpu_pdev->dev.of_node` (the `qcom,kgsl-3d0` platform device) rather than
`gmu->dev->of_node`.

## 3. What this means

The project's own KGSL-vs-DRM comparison listed the remaining differences as:

> Neither **quirk parsing (cx-gdsc set in DT, ignored)** nor ACD nor the bus
> vote exists in our tree.

ACD and the bus vote have since been added. The last one standing is the
**cx-gdsc quirk** — and touching the CX GDSC's mode, which is what the quirk
governs, **kills the GPU's secure firmware immediately**.

Combined with the other thread from this session — that the dominant crash in
the entire project archive (167/213) is an **EL2/HYP data abort in the GPU's
secure side** — the shape of the problem is now:

- The GPU's secure firmware aborts, and it aborts *readily*: a regulator mode
  change on the CX GDSC is enough to trigger it.
- That same abort is the project's most common crash by a wide margin.
- The project's candidate D ("secure gate") was dismissed with an instrument
  (a CP register) that structurally **cannot** see a fault in the hypervisor.

So the secure side is not a side-issue: it is the most probable seat of the
defect, and it has never been instrumented, because everything the project can
read lives in the non-secure world.

## 4. Next steps

1. **Fix the quirk read** (GPU node, not GMU node) and re-run 274c with the
   quirk honoured — i.e. do the mode change the way KGSL does (gated on the
   quirk, in KGSL's order). One character of DT + a few lines.
2. **Try the opposite mode.** KGSL sets `REGULATOR_MODE_IDLE` on the suspend
   path and NORMAL on resume. If NORMAL kills it, IDLE may be what the GPU
   wants in this state — and that would be a *cause*, not just a crash.
3. **Look for the secure-side reason.** The HYP log is encrypted, but the
   project's `ANDROID-KGSL-PSP-REFERENCE.md` has a working KGSL reference where
   "GMU firmware boots with no SMMU panic". The diff between that and here is
   the highest-value comparison left.

## 5. Operational notes (all re-confirmed this session)

- The **last byte of the 512-byte cmdline field is lost** — never put a
  parameter's value there.
- **A file on the phone is not evidence**: `/userdata` rolls back on reset.
  Stream the dmesg to the host over SSH instead.
- **Kill stale `dmesg -W` readers on the device** before opening new SSH
  sessions: two of them held sessions and made the device appear unreachable.
- `pkill -f <pattern>` matches your *own* shell if the pattern appears in your
  command line. Use the bracket trick (`retry274[c]`) or match by port owner.
