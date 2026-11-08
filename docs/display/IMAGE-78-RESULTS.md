# Image 78 — WHICH CLOCK STALLS: answered, and image 76's conclusion was WRONG

## Answer: no clock stalls. All five prepare and enable cleanly.

```
--- resolved GMU clock list: count=5 ---
clock[0] 'gmu_clk'    clk=0000000065dcbc09
clock[1] 'cxo_clk'    clk=0000000023164afa
clock[2] 'axi_clk'    clk=00000000a92f9038
clock[3] 'memnoc_clk' clk=00000000bfb6fcf1
clock[4] 'gpu_cc_ahb' clk=000000008bd72dcc
--- pass 1: clk_prepare ---   all five ret=0
--- pass 2: clk_enable ---    all five ret=0
clocks enabled (ret=0)
GMU IRQ enabled
boot status read (cold)
```

Every clock resolved to a distinct non-NULL pointer and **every** `clk_prepare`
and `clk_enable` returned 0. So `clk_bulk_prepare_enable()` is **not** the hang.

**Correction to image 76.** That image's last marker was `clk_set_rate done`, and
I concluded the resume "stalls inside `clk_bulk_prepare_enable`". That inference
was drawn from the *absence* of the next marker during a capture I have since
proven unreliable — the `/dev/kmsg` reader stalls, and `/dev/kmsg` plus device
trace files both failed independently. The absence of a marker was not evidence
of a hang. Image 78, with the log volume reduced, shows those steps actually
succeed. Image 76's conclusion was a capture artefact and is retracted.

This also **weakens the `msm_clk_bulk_get` under-allocation theory**: all five
`clk` pointers are sane here, so the kmalloc-64 slack absorbed the overwrite
without an observable fault. The bug is still real and still worth fixing, but it
is not what stopped us.

## New, precise failure: Oops writing the RPMh sequence register

Immediately after `boot status read (cold)`:

```
Unable to handle kernel paging request at virtual address ffffff801ff90000
Mem abort: DABT (current EL), IL = 32 bits
Data abort: ISV = 0, ISS = 0x00000046  CM = 0  WnR = 1     <- a WRITE
[ffffff801ff90000] *pgd=00000000b23fe003, *pud=00000000b23fe003, *pmd=0000000000000000
Internal error: Oops: 96000046 [#1] PREEMPT SMP
Process bash (pid: 1108)
pc : msm_writel+0x48/0x60
lr : msm_writel+0x24/0x60
x20: 00000000febea1e1
```

`0xfebea1e1` is precisely the constant written to `REG_A6XX_PDC_GPU_SEQ_MEM_0` by
`a6xx_gmu_rpmh_init()` (`pdcseq_write(gmu, REG_A6XX_PDC_GPU_SEQ_MEM_0, 0xfebea1e1)`).

So the fault is in **the `pdcseq_write()` path added for the Samsung PDC split**:

```c
static inline void pdcseq_write(struct a6xx_gmu *gmu, u32 offset, u32 value)
{
	return msm_writel(value, gmu->seq_mmio + (offset << 2));
}
```

`gmu->seq_mmio` evaluates to an **unmapped kernel virtual address** at this point,
even though the probe completed successfully:

```
after mmio map -> after pdc_mmio map -> after seq_mmio map -> ... -> PROBE COMPLETE
```

and `gmu->seq_mmio` passed the `IS_ERR()` check. So either:

1. the ioremap never produced a usable mapping (the `kgsl_gmu_pdc_seq` region at
   `0xb480000` may not be mappable from the normal world), or
2. `gmu->seq_mmio` was corrupted between the probe and this write.

`a6xx_gmu_get_mmio()` uses `devm_ioremap()` and only checks for a non-NULL return,
not for whether the mapping is actually usable — a plausible route to (1).

Note the writes immediately *before* this one, which use `gmu->mmio`, did **not**
fault, so `gmu->mmio` is good and only the seq mapping is bad.

## Next diagnostic when approved

Print the three pointers twice — once immediately after mapping in the probe, once
immediately before `a6xx_gmu_rpmh_init()` — and compare. That distinguishes
"never a valid mapping" from "valid then corrupted", which the current evidence
cannot separate.