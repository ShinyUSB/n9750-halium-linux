# ROOT CAUSE — GMU firmware faults on unmapped IOVA, watchdog bites, recovery resume times out

Date: 2026-10-10. Kernel `#133` (image 191). Evidence from the on-device capture
log `/var/log/gpu191.log`, current boot only (separated by `CAPTURE START`).

## The chain, in order, with timestamps

```
[  133.93] POST_START: fw_start begin
[  134.04] POST_START: fw_start end ret=0          <-- bring-up genuinely SUCCEEDS
[  134.04] === GMU BOOTSTRAP BEGIN ===
[  134.04] HFI: verify_fw_version enter
[  134.04] HFI-B fw_version post-write q0 rd=0 wr=2 doorbell=0x01 data=00100201 10010000
[  134.04] HFI-SNAP C+1ms q0 rd=0 wr=2 q1 rd=0 wr=0 INTR_INFO=0x0 RAW=0x0 INFO0=0x0
[  134.048] arm-smmu kgsl-smmu: FAR=0x820  FSR=0x40000402 [TF R SS] TTBR0=0x00020003e69a7000
[  134.048]   Unhandled context fault: iova=0x820, cb=1, fsynr0=0x60063
[  134.049]   soft iova-to-phys=0x0
[  134.049]   SOFTWARE TABLE WALK FAILED! unmapped address
[  134.049]   SID=0x5
[  134.049]   ... same for iova=0x800, then iova=0x280 ...
[  135.54] platform 2c6a000.qcom,gmu: GMU watchdog expired
[  135.54] WDT139: AO_STATUS=0x1 NMI=0x0 INITRES=0xbabeface FWBUSY=0x0
[                    SPTPRAC=0xc0 RPMHPWR=0x0 OOB_H2G=0x0
[  135.54] RECOVER166: GMU WDT bite, scheduling recovery
[  135.54] RECOVER166: begin (force off + cold resume)
[  135.55] failed to power off SPTPRAC: 0xc0
[  135.55] RECOVER166: GMU halted, resuming
[  135.55] BC132: ev=RESUME_ENTRY depth=2           <-- nested resume
[  135.55] POST_START: fw_start begin
[  136.69] POST_START: fw_start end ret=-110        <-- recovery resume TIMES OUT
[  136.69] a6xx_gmu_resume RETURNING ret=-110
[  136.69] RECOVER166: end ret=-110
```

Then `get_param` returns `ENXIO` and Mesa falls back to llvmpipe.

## Root cause

**The GMU firmware cannot read its own memory: it issues SMMU requests for IOVA
`0x280`, `0x800`, `0x820`, and nothing in the GMU address space is mapped there.**

The GMU's complete IOVA map, logged by the driver immediately before `fw_start`:

```
SMMU map hfi     iova=0x60001000 size=0x5000
SMMU map icache  iova=0x4000    size=0x3c000   -> 0x4000..0x40000
SMMU map dcache  iova=0x44000   size=0x3c000   -> 0x44000..0x80000
SMMU map dummy   iova=0x60000000 size=0x1000
SMMU map sentinel iova=0xdeadb000 size=0x1000
SMMU map log     iova=0x60006000 size=0x1000
SMMU gmu fwspec SID[0]=0x4000005
```

**The lowest mapping is `0x4000`.** Every faulting address (`0x280`, `0x800`,
`0x820`) is below it, in a hole that has no mapping at all. `soft iova-to-phys=0`
and "SOFTWARE TABLE WALK FAILED" confirm the page walk finds nothing.

Because the firmware cannot fetch what it needs immediately after
`fw_start` returns, it never completes init — hence `INITRES=0xbabeface` (the
CA/vendor "bad face" init-failure magic). 1.5 s later the GMU watchdog expires.
Our own `recover166` handler then does a force-off + cold resume, but the GPU is
already wedged (`failed to power off SPTPRAC: 0xc0`, `RPMHPWR=0x0`) and that
resume times out at `-ETIMEDOUT`. **The recovery resume is what actually drops
the GPU**, and it is a consequence, not the cause.

So the real chain is:

```
missing IOVA mapping below 0x4000
  -> firmware cannot read own memory, INITRES=0xbabeface
  -> GMU watchdog expires (~1.5s)
  -> recover166 cold resume on a wedged GPU times out (-110)
  -> ENXIO -> Mesa -> llvmpipe
```

## A theory I had to discard (recorded so it is not retried)

My first hypothesis was "our driver omits the `GMU_ITCM` (`0x00000`, 16K) and
`GMU_DTCM` (`0x40000`, 16K) regions that CAF's `gmu_vma[]` lists, so map them
likewise." **The in-tree CAF KGSL driver disproves this.** `kgsl_gmu.c`:

```c
static int alloc_and_map(...)
{
        if (md->mem_type == GMU_ITCM || md->mem_type == GMU_DTCM)
                return 0;               /* no memory allocated, no mapping */
```

CAF reserves those two VMAs but deliberately maps no memory behind them. So the
faults at `0x280`/`0x800`/`0x820` are **not** a missing-ICTM/DTCM-mapping bug,
and adding those mappings would be wrong.

### Second theory, also closed: we never write ICACHE_CONFIG/DCACHE_CONFIG

CAF writes three registers at the top of its idle-config path
(`drivers/gpu/msm/adreno_a6xx_gmu.c:302-304`):

```c
gmu_core_regwrite(device, A6XX_GMU_SYS_BUS_CONFIG, 0x1);
gmu_core_regwrite(device, A6XX_GMU_ICACHE_CONFIG, 0x1);
gmu_core_regwrite(device, A6XX_GMU_DCACHE_CONFIG, 0x1);
```

This looked very promising, because the comment in our `a6xx_gmu_power_config()`
said "our port never did" — an admitted gap. **It is already fixed in our tree.**
`adreno/a6xx_gmu.c:1052-1056` writes all three, instrumented.

The register addresses differ from CAF by exactly the documented
`gmu2gpu_offset`:

```
0x1F400 - 0x4C00 = 0x1A800     (ICACHE_CONFIG)
0x23B10 - 0x9310 = 0x1A800     (AO_AHB_FENCE_CTRL)
```

and the offset was cross-checked against `GENERAL_7`, which matches exactly. So
the GMU register base is right and this theory is dead. (Whoever added that
instrumentation already made this fix; the comment above it is stale.)

## What is proven vs. what is still open

**Proven by the log:** the faulting addresses, that no mapping covers them, the
`0xbabeface` init failure, the watchdog bite, and that the recovery resume times
out and drops the GPU.

**Not yet established:** *why* the firmware reads `0x800`/`0x280`. It runs from
ITCM via a private MMIO aperture (`REG_A6XX_GMU_CM3_ITCM_START`), which should
not generate SMMU traffic at all. The fact that it does suggests the firmware is
in a state where it believes it is executing from system memory, i.e. it is
already lost before these faults — the mapping hole may be a *symptom* of a
firmware that failed earlier, not the initiating cause. `INITRES=0xbabeface` is
the last known-good state indicator and it is already failed.

This distinction matters: **I am not claiming the fix is "map IOVA 0x0".** The
honest next question is why `fw_start` returns success while leaving the firmware
unable to fetch. The very first thing after `fw_start` succeeds is our own
`a6xx_hfi_start()` / `verify_fw_version`, which is the first code to touch the
GMU in this state.

## Next step (do not build 192 yet)

1. Diff our `a6xx_gmu_fw_start()` against CAF's `kgsl_gmu_start()` (in-tree at
   `drivers/gpu/msm/adreno_a6xx_gmu.c`) for anything CAF does *between* the
   `fw_start` write and first HFI traffic. The cache-config registers are
   already handled, so the difference is elsewhere.
2. Read back the GMU `AO_STATUS` / `INITRES` registers **before** our
   `a6xx_hfi_start()` runs, i.e. immediately after `fw_start` returns 0. If
   `INITRES` is already `0xbabeface` at that instant, the firmware was never
   viable and the SMMU faults are pure symptom — which would mean the bug is
   entirely inside `fw_start` and the HFI step is innocent. This is a single
   cheap read and it cleanly separates the two hypotheses.
3. Only then consider a mapping change.

The submit path (images 190/191) is correct and is not implicated here.

## Cheap next probe (no flash, no build)

Step 2 is a few lines added to `a6xx_gmu_resume()` right after
`fw_start` returns success. It can be validated on the live device via the
existing capture harness. That single read decides whether we are chasing
`fw_start` or `hfi_start`, and I would rather spend the next cycle on that than
on another speculative image.