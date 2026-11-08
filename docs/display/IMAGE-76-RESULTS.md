# Image 76 — GMU resume stall localised

## Marker sequence (authoritative host capture)

```
1  A6XX_TRACE: enter a6xx_pm_resume
2  A6XX_TRACE: pm_runtime_get_sync done
3  A6XX_TRACE: clk_set_rate done
<<<nothing further — stream ends here>>>
```

Corresponding source in `a6xx_gmu_resume()`:

```c
pm_runtime_get_sync(gmu->dev);            /* marker 2 reached */
clk_set_rate(gmu->core_clk, 200000000);   /* marker 3 reached */
ret = clk_bulk_prepare_enable(gmu->nr_clocks, gmu->clocks);
                                       /* <-- STALLS HERE, never returns */
A6XX_TRACE("clocks enabled (ret=%d)", ret);
```

## Conclusion

**B — the GMU resume does not complete silently. It stalls inside
`clk_bulk_prepare_enable(gmu->nr_clocks, gmu->clocks)`.**

The device does **not** crash: uptime 64 s and climbing, `EXT4-fs error` count 0,
oops count 0, no reboot. It is a genuine hang in a non-sleeping clock path.

## Two markers that are correctly absent, and why

* `enter msm_gpu_hw_init` never prints. That is **expected, not a gap**:
  `adreno_load_gpu()` does `pm_runtime_get_sync()` *before* `msm_gpu_hw_init()`,
  and the runtime-PM resume is what calls `a6xx_pm_resume()`. So the hang occurs
  before `msm_gpu_hw_init()` is ever entered.
* `msm_gpu_hw_init` markers, the `out:` error-path marker, and the
  `a6xx_gmu_resume RETURNING` marker are all downstream of the stall.

## Ruled out

* **Clock-name mismatch.** `a6xx_gmu_clocks_probe()` asks
  `msm_clk_bulk_get_clock(..., "gmu")`, and that helper tries both `"gmu"` and
  `"gmu_clk"`. The DT supplies `"gmu_clk"`, so `gmu->core_clk` resolved. This is
  *not* another instance of the reg-name/IRQ-name defect class.
* **`clk_set_rate` on a NULL clock.** Marker 3 printing shows `clk_set_rate`
  returned; and `core_clk` is non-NULL per above.
* **Crash / panic / filesystem.** All zero.

## The five clocks being enabled, in DT order

```
gmu_clk   = GPU_CC_CX_GMU_CLK     (clock_gpucc)
cxo_clk   = GPU_CC_CXO_CLK        (clock_gpucc)
axi_clk   = GCC_DDRSS_GPU_AXI_CLK (clock_gcc)
memnoc_clk= GCC_GPU_MEMNOC_GFX_CLK(clock_gcc)
gpu_cc_ahb= GPU_CC_AHB_CLK        (clock_gpucc)
```

## Hypothesis — UNPROVEN, explicitly

Three of the five are in the GPU_CC / CNOC domain, and the same DT node carries
a power vote we are not making:

```
regulator-names = "vddcx", "vdd";
vddcx-supply    = <&gpu_cx_gdsc>;
vdd-supply      = <&gpu_gx_gdsc>;
qcom,msm-bus    = "cnoc" with on/off vectors
mboxes = <&qmp_aop 0>;  mbox-names = "aop";   /* ACD enable/disable */
```

`a6xx_gmu_resume()` calls `pm_runtime_get_sync(gmu->dev)`, but the GMU probe only
did `pm_runtime_enable()` with no `pm_ops` of its own, so no runtime-PM callback
takes the CNOC / GPU_CX power vote. Enabling clocks inside a still-collapsed
domain is a plausible way to wedge a clock-gating register write.

This is reasoning from the DT, **not** something the capture has proven.

## Next step when approved

Instrument `clk_bulk_prepare_enable` per-clock — print the index and `cl->id`
before and after each `prepare_enable` — to identify **which specific clock**
stalls. That distinguishes a bad clock handle from a missing power vote, at the
cost of one more image.

Also worth checking at the same time: whether the vendor KGSL GMU driver takes an
explicit CNOC/GPU_CX power vote before enabling these same clocks.