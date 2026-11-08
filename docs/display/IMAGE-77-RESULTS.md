# Image 77 — instrumentation correct, capture channel inadequate

## What image 77 contains (built, verified, flashed)

Per-clock tracing installed **only** at the `a6xx_gmu_resume()` call site, via a
local A6xx-only helper `a6xx_gmu_clk_bulk_prepare_enable_traced()`. It mirrors
this kernel's two-pass structure (`clk_bulk_prepare()` then `clk_bulk_enable()`),
does not wrap or replace the generic helper, and changes no clock or power
behaviour. `a6xx_gmu_reset()` was deliberately restored to the original
`clk_bulk_prepare_enable()` after I caught a bad scripted edit.

| check | result |
|---|---|
| build | 0 errors, 0 warnings |
| SDE/DSI/panel diff vs production | 0 |
| DT unchanged | yes (pre-existing 14-line `gmu` phandle only) |
| IOMMU adapter unchanged | `msm_gpu_mmu.c` `fac06af5…`, `msm_iommu.c` `18b890cc…` |
| firmware parser unchanged | `adreno_gpu.c` `51f5445199…` |
| production tree | untouched |
| deferred-GMU timing | unchanged |
| image 77 | `8f7353e9d25c7bca3746a61b26fefd5d1181e3e2a39741b2617a509af8228773` |

## Question 1 — WHICH CLOCK STALLS? **NOT ANSWERED**

The instrumentation is correct and present in the binary (verified via `strings
vmlinux`), but **every capture attempt failed**. This is a capture-layer failure,
not a device or code failure.

Measured failure modes:

1. **`/dev/kmsg` reader stalls at ~8900–10000 records.** It replays the *entire*
   existing ring buffer from the start, so a stream opened at 56 s uptime has
   already consumed ~8500 records before a trigger even fires. Image 76 opened
   at ~30 s and therefore had headroom — which is exactly why image 76 captured
   the stall and image 77 did not. Same binary behaviour, different timing.
2. **`sec_ts` prints every ~9 ms (~110 lines/s)** with no module parameter to
   throttle it (`sec_ts_read_event()` is called unconditionally from the
   touchscreen IRQ in `drivers/input/touchscreen/sec_ts/y771/sec_ts.c`). The
   8192-line ring buffer therefore rotates A6XX_TRACE out in ~70 s.
3. **The device reboots within seconds of the trigger**, destroying the ring
   buffer before any host round-trip can sample it.
4. **Host polling is too slow**: each `dmesg` over SSH costs ~3–10 s against a
   flooded buffer; measured 10 s/poll, versus a ~1–3 s window.
5. **Device-side trace files do not persist.** `/root/a6xx-trace.log` and the
   earlier `/root/gpu-trace.log` are both **empty or gone after reboot**, even
   though `/lib/firmware` and `/root/a6xx-fw-orig` from an earlier boot *do*
   persist. So `/root` writes are not durable here, and I no longer have a
   reliable on-device recording channel. (The earlier `gpu-trace.log` loss was
   blamed on the corrupt filesystem; that explanation is now doubtful.)

Retrying does not converge on an answer, so I stopped.

## Question 2 — WHAT VOTE DOES KGSL MAKE BEFORE ENABLING THE CLOCKS? **ANSWERED**

From `kgsl_gmu.c` → `gmu_start()`, identical in the `KGSL_STATE_INIT`,
`KGSL_STATE_SLUMBER` and `KGSL_STATE_RESET` cases:

```c
gmu_aop_send_acd_state(device);   /* AOP/QMP ACD mailbox */
gmu_enable_gdsc(gmu);             /* regulator_enable(gmu->cx_gdsc) */
gmu_enable_clks(device);          /* <- only now the clocks           */
gmu_dev_ops->irq_enable(device);  /* GMU IRQ                           */
rpmh_gpu_pwrctrl(..., GMU_FW_START, GMU_COLD_BOOT, 0);
hfi_start(device, gmu, GMU_COLD_BOOT);
```

| resource | mechanism | DT binding |
|---|---|---|
| AOP/QMP | `gmu_aop_send_acd_state()` | `mboxes = <&qmp_aop 0>`, `mbox-names = "aop"` |
| GPU_CX / vddcx | `gmu_enable_gdsc()` → `regulator_enable()` | `vddcx-supply = <&gpu_cx_gdsc>` |
| CNOC | `gmu_cnoc_bw_probe()` → `build_rpmh_bw_votes()`, applied by `msm_bus_scale_client_update_request(gmu->pcl, …)` | `qcom,msm-bus = "cnoc"` |
| GDSC | CX only here; GX handled in pwrctrl | `vdd-supply = <&gpu_gx_gdsc>` |
| RPMh | `rpmh_gpu_pwrctrl(GMU_FW_START)` — after clocks | — |

**Our `a6xx_gmu_resume()` does none of this.** It calls only
`pm_runtime_get_sync(gmu->dev)`, and the GMU probe registered `pm_runtime_enable()`
with no `pm_ops`, so no runtime-PM callback ever enables the CX GDSC or sends the
ACD. The clocks are enabled inside a GDSC that KGSL would have powered first.

## Additional finding — clock array under-allocation (upstream bug)

`msm_clk_bulk_get()` in `msm_drv.c`:

```c
local = devm_kcalloc(dev, sizeof(struct clk_bulk_data *), count, GFP_KERNEL);
local[i].id = devm_kstrdup(dev, name, GFP_KERNEL);
ret = devm_clk_bulk_get(dev, count, local);
```

`sizeof(struct clk_bulk_data *)` is **8**, but `struct clk_bulk_data` is **16**
(`id` + `clk`), and the kernel helper `clk_bulk_get()` walks it with 16-byte
stride. For the 5 GMU clocks: **40 bytes allocated, 80 bytes written.** Entries
2–4 fall outside the kmalloc-64 object. Verified identical in upstream v4.19, so
this is an upstream defect, not a divergence we introduced.

KGSL avoids it entirely: `struct clk *clks[MAX_GMU_CLKS]` (6 fixed slots) filled
by `devm_clk_get()`, enabled with `while (j < MAX_GMU_CLKS && gmu->clks[j])` —
i.e. it **stops at the first NULL**, whereas the bulk API never NULL-checks.

This is a plausible cause of a hang inside `clk_bulk_prepare_enable()`, and it is
independent of the missing GDSC vote. Both remain candidates; neither is proven.

## Options to actually answer "which clock"

* **Suppress the `sec_ts` spam** (rate-limit or gate those printk). Cuts log
  volume ~100x and makes the existing kmsg capture trivially reliable. It is an
  unrelated driver, and outside the "do not touch" list, but it is a real change.
* **Fix the under-allocation** — `sizeof(struct clk_bulk_data)` instead of
  `sizeof(struct clk_bulk_data *)`. Genuine upstream bug fix, directly on the
  suspected path. The user asked to report before fixing, so not applied.
* **Add a power vote before the clocks** — enable `vddcx` via
  `regulator_enable()` (and ACD), mirroring `gmu_enable_gdsc()`. This is the
  behaviour change upstream/KGSL rely on.