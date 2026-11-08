# A640 DRM bring-up — runtime results

Full boot log: `deferred-gmu-boot.log` (streamed host-side from `dmesg -w`).

## Why the kernel looked silent

For images 67/68/69 there was **no log channel at all**:

| sink | state |
|---|---|
| kernel `console=` | **absent** — cmdline only has `androidboot.console=ttyMSM0`, which is an Android property, not a kernel param. Every printk since boot was discarded. |
| ramoops / pstore | correctly configured (`CONFIG_PSTORE_RAM=y`, DT-reserved `0xc1300000/0x100000`) but **only writes on panic/oops**. A hang writes nothing. |
| reset reason | Samsung `pet_watchdog` / hardware watchdog, which also records nothing. |
| `/dev/mem` | not exposed in TWRP, so the reserved region cannot be read manually. |

Two dead ends worth recording:

* **Image 69 was not new information.** The cmdline was set to
  `a6xx.gpu_init_delay_ms=300000`, but `a6xx_gmu.o` is a composite object inside
  the built-in `msm_drm`, so `KBUILD_MODNAME` supplied the prefix and the param
  concatenates **without a dot** on this kernel vintage. The name that actually
  parsed was `a6xxgpu_init_delay_ms`. Verified by `strings vmlinux`; fixed by
  pinning `MODULE_PARAM_PREFIX` to `"a6xx."`. Image 70 behaves as intended.
* **Device-side log files do not survive.** `/tmp` is a tmpfs overlay and `/data`
  is reset on reboot — a capture written there was gone after the next reset.
  The working approach is to stream `dmesg -w` over SSH **to the host**, so the
  bytes already received survive whatever the device does next.

## Image 70 — results

### GMU probe (deferred, ran at 340.96 s): PASSED

```
a6xx: deferring GMU probe by 300000ms        (35.26 s)
a6xx: deferred GMU probe starting now        (340.96 s)
iommu: Adding device 2c6a000.qcom,gmu:gmu_kernel to group 53
cb 'gmu_kernel' ... attached, domain=...
GMU using context bank gmu_kernel, domain=...
Unable to set the OPP table for the GMU      (warning only)
```

Negative evidence that matters more than the positive lines:

| string | count | meaning |
|---|---|---|
| `Unable to find` (gmu/gmu_pdc registers) | **0** | all **3** MMIO lookups succeeded |
| `Unable to get interrupt` | **0** | **both** IRQs acquired |
| `Domain not attached; cannot detach!` | **0** | err path not taken — Blocker 2 warning is **gone** |
| `deferred GMU probe failed` | **0** | probe returned 0 — **success** |
| `Internal error` / `Unable to handle` | **0** | no oops |

Corroborated out-of-band: no kworker is spinning (all ~0 CPU time, all in
sleepable state), so the probe *returned* rather than wedging. System stayed up
past 460 s with zero panics and zero reboots.

### GPU core probe (at 35–36 s): FAILED on missing firmware

```
cb 'gfx3d_user' attached, domain=...
aspace 'gfx3d' on cb 'gfx3d_user': iova 0x0-0x7fffffffff
GPU address space ready on gfx3d_user
loading .../qcom/a630_sqe.fw failed with error -74
loading .../firmware-modem/image/a630_sqe.fw failed with error -74
failed to load a630_sqe.fw
failed to load firmware: -2
```

Second probe attempt then trips over the first:

```
Unbalanced pm_runtime_enable!
msm_drm ae00000.qcom,mdss_mdp: failed to request IRQ487: -16   (-EBUSY)
msm_drm ae00000.qcom,mdss_mdp: failed to load adreno gpu
```

`-EBUSY` on IRQ 487 is fallout: attempt 1 claimed it, then failed on firmware and
did not release it.

## State of the blockers

| blocker | status |
|---|---|
| Blocker 1 — GMU MMU reg-name mismatch | **FIXED and verified at runtime** |
| Blocker 1b — GMU IRQ name mismatch | **FIXED and verified at runtime** |
| Blocker 2 — `Domain not attached` | **no longer reachable**; warning absent |
| `a630_sqe.fw` on device | **OPEN** — not present, `-74`/`-2` |
| `a640_gmu.bin` on device | **OPEN** — not present; not yet reached |
| bootloop on immediate GMU probe | **OPEN** — deferred probe is stable; cause still unexplained |