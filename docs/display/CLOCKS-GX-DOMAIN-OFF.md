# BREAKTHROUGH — the GPU's GX (graphics) clock domain is OFF

Date: 2026-10-12
Source: `/sys/kernel/debug/clk/clk_summary` on the live device (no rebuild needed)
Reference device tree: `arch/arm64/boot/dts/qcom/sm8150-gpu.dtsi` (already in our tree)

## The measurement

Column header is `enable count | prepare count | rate | accuracy | duty phase | duty cycle`.

```
 gpu_cc_cx_gmu_clk     en=1  prep=1  rate=200000000     <- CX (CPU/context) domain: ON
 gpu_cc_gx_gmu_clk     en=0  prep=0  rate=200000000     <- GX (graphics) domain: OFF
 gcc_gpu_gpll0_clk_src en=0  prep=0  rate=600000000     <- GPU core PLL source: OFF
 gpu_cc_ahb_clk        en=2  prep=2  rate=0
 gpu_cc_cxo_clk        en=1  prep=1  rate=0
 gcc_ddrss_gpu_axi_clk en=1  prep=1  rate=0             <- GPU DRAM AXI: rate 0
 gcc_gpu_memnoc_gfx_clk en=1 prep=1  rate=0             <- GPU memnoc GFX: rate 0
 gpu_cc_gx_vsense_clk  en=0  prep=0  rate=0
 gpu_cc_gx_qdss_tsctr_clk en=0 prep=0 rate=0
 measure_only_gpu_cc_gx_gfx3d_clk   en=0 prep=0 rate=1000
 measure_only_gpu_cc_cx_gfx3d_clk   en=0 prep=0 rate=1000
```

Every **CX** (CPU/context) clock is enabled. Every **GX** (graphics) clock is
disabled. The GPU core PLL source (`gcc_gpu_gpll0_clk_src`, 600 MHz) has
enable count 0.

## Why this explains everything

Adreno splits its clock domains:

- **CX** = CPU / context-switch domain. Runs the CP, command processor, ring
  fetches, `CP_EVENT_WRITE`.
- **GX** = graphics domain. Runs the RB, SP, UCHE — i.e. everything that
  actually *writes pixels*.

That maps exactly onto our symptoms:

| observation | explanation |
|---|---|
| CP_SCRATCH(2) and ring fence advance 1,2,3… | CX domain is clocked |
| Mesa's nested IB executes | CX domain is clocked |
| `CP_EVENT_WRITE` timestamps land in control memory | CX domain is clocked |
| `RBBM_CLOCK_CNTL=0x8aa8aa02` looks "correct" | that's an *internal* register; meaningless if the GX gate above it is shut |
| 2D `CP_BLIT` produces no write | **GX domain not clocked** |
| shader-based `util_blitter` render pass produces no write | **GX domain not clocked** |
| all of the above, independent of UBWC / sysmem / gmem | none of those touch the clock gates |

This retires the internal-clock hypothesis (`CLKCHK` reading `0xdeadbead` etc.)
as a dead end — those registers are fine, the *gate* above them is closed.

## Why it is closed, and why that is expected

From the device tree `sm8150-gpu.dtsi`, the `qcom,kgsl-3d0` node requests:

```dts
clocks = <&clock_gpucc GPU_CC_CXO_CLK>,
         <&clock_gcc GCC_DDRSS_GPU_AXI_CLK>,      /* mem_clk */
         <&clock_gcc GCC_GPU_MEMNOC_GFX_CLK>,      /* mem_iface_clk */
         <&clock_gpucc GPU_CC_CX_GMU_CLK>,         /* gmu_clk  <-- CX, not GX */
         <&clock_gpucc GPU_CC_AHB_CLK>,
         <&clock_cpucc L3_GPU_VOTE_CLK>;
```

Note **`GPU_CC_CX_GMU_CLK`** — the CX GMU. `GPU_CC_GX_GMU_CLK` is *not* in the
list. On this device it is normally switched on by **KGSL**, via the GMU/RPMH
power-up path. We are running **msm/drm (freedreno), not KGSL** — and CAF's
`a6xx` driver contains **no `devm_clk_*` calls at all**:

```
$ grep -rc "devm_clk" adreno/*.c     # no matches
```

It relies entirely on the GMU firmware + RPMH to bring the GX domain up, and
that path is evidently not doing it.

## Next moves, in order

1. **Confirm the GPU's GDSC/power state.** The DT also carries
   `qcom,gpu-quirk-cx-gdsc` and `qcom,gpu-quirk-secvid-set-once`; KGSL has
   corresponding GDSC workaround code. Check whether the GPU power domain is
   actually on (RPMH `CX_GMU_RPMH_POWER_STATE` readbacks exist in our driver).
2. **Force-enable `GPU_CC_GX_GMU_CLK` from the kernel** and see whether
   rendering starts. This is a one-line experiment in `a6xx_hw_init`
   (`clk_prepare_enable()` on the GX GMU clock) and would confirm the diagnosis
   outright — but must be done carefully, since the clock is not in the DT
   `clocks` list for this node, so it would need `of_clk_get()` by name from a
   node that doesn't request it (likely needs a DT change, or a direct
   `devm_clk` on a new probe).
3. **Compare against KGSL's power-up** (still the strongest reference): what
   does KGSL do that CAF msm does not, specifically around GX clock gating and
   the `cx-gdsc` quirk.

## Method note

This took no rebuild at all — the answer was in a sysfs file the whole time.
Five images of instrumented kernel dumps were needed to get here, and the
decisive datum was a clock framework summary that nobody had thought to read.

Worth remembering: before instrumenting a driver hard, enumerate the
*platform* facts — clocks, power domains, regulators, buses. The driver is only
one layer, and it turned out to be doing its job correctly the whole time.