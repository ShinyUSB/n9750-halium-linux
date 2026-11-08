# FINDING: the submission path is missing upstream's page-table prologue

Date: 2026-10-18
Method: source-only diff of the a6xx submission path against upstream Linux.
No hardware experiment. Baseline image 269.

## Headline

Upstream `a6xx_submit()` calls `a6xx_set_pagetable()`, which on **every context
change** emits a six-step prologue to the CP. **Our tree emits none of it.**

This is the strongest lead found in the entire investigation, and — importantly —
it is a **known gap that was identified and then never implemented**, then
incorrectly retracted.

## What upstream emits, and that it is not chip-gated

`a6xx_gpu.c:233-360`, `a6xx_set_pagetable()`:

```c
if (ctx->seqno == ring->cur_ctx_seqno)
        return;
...
/* Turn off protected mode to write to special registers */
OUT_PKT7(ring, CP_SET_PROTECTED_MODE, 1);
OUT_RING(ring, 0);

OUT_PKT4(ring, REG_A6XX_RBBM_PERFCTR_SRAM_INIT_CMD, 1);
OUT_RING(ring, 1);

/* Execute the table update */
OUT_PKT7(ring, CP_SMMU_TABLE_UPDATE, 4);
OUT_RING(ring, CP_SMMU_TABLE_UPDATE_0_TTBR0_LO(lower_32_bits(ttbr)));
OUT_RING(ring, CP_SMMU_TABLE_UPDATE_1_TTBR0_HI(upper_32_bits(ttbr)) |
                  CP_SMMU_TABLE_UPDATE_1_ASID(asid));
OUT_RING(ring, CP_SMMU_TABLE_UPDATE_2_CONTEXTIDR(0));
OUT_RING(ring, CP_SMMU_TABLE_UPDATE_3_CONTEXTBANK(0));

OUT_PKT7(ring, CP_MEM_WRITE, 5);      /* TTBR0 -> memstore, for debug/preempt */

OUT_PKT7(ring, CP_EVENT_WRITE, 1);
OUT_RING(ring, CACHE_INVALIDATE);

OUT_PKT7(ring, CP_WAIT_REG_MEM, 6);   /* wait on PERFCTR_SRAM_INIT_STATUS */

if (!adreno_is_a7xx(...) && !adreno_is_a8xx(...))
        OUT_PKT7(ring, CP_SET_PROTECTED_MODE, 1);  /* back to 1 */
OUT_RING(ring, 1);
```

The `CP_SET_PROTECTED_MODE` and `CP_THREAD_CONTROL` steps **are** wrapped in
`adreno_is_a7xx()/a8xx()` checks — but `CP_SMMU_TABLE_UPDATE`, `CP_MEM_WRITE`,
`CACHE_INVALIDATE` and the `CP_WAIT_REG_MEM` are **unconditional**, i.e. emitted
for A6xx too. (a6xx_gpu.c:297-305, 314, 329-351)

## The packet exists on A640

```
mesa-pm4.xml:  <value name="CP_SMMU_TABLE_UPDATE" value="0x53" variants="A5XX-"/>
```

`variants="A5XX-"` means "A5XX and later" — so it is available on A6xx, and the
opcode is already defined in our own header (`adreno_pm4.xml.h:208`,
`CP_SMMU_TABLE_UPDATE = 83`).

## Our tree emits none of it

| step | upstream | ours |
|---|---|---|
| `CP_SET_PROTECTED_MODE 0/1` | yes | **never on a6xx** (only `a5xx_power.c`) |
| `RBBM_PERFCTR_SRAM_INIT_CMD` | yes | **never** |
| `CP_SMMU_TABLE_UPDATE` | yes | **never** |
| `CP_MEM_WRITE` (TTBR0→memstore) | yes | **never** |
| `CACHE_INVALIDATE` after pgtable | yes | Mesa emits its own; kernel does not |
| `CP_WAIT_REG_MEM` on SRAM_INIT_STATUS | yes | **never** |

Our submit path emits, in total: `CP_REG_TO_MEM` (stats), `CP_EVENT_WRITE`
(fence), `CP_INDIRECT_BUFFER_PFE`, `CP_ME_INIT`. No prologue.

## Why the corpus retracted this — and why the retraction is wrong

`PLAN-GPU-ZERO-TO-FULL.md` / the consults list `CP_SMMU_TABLE_UPDATE` as
**"v5.10+ only"** and rule it out.

That is contradicted by two independent checks:
1. **The packet spec** marks it `A5XX-`, i.e. it predates v5.10 by generations.
2. **Upstream's own code** emits it unconditionally for A6xx — the a7xx/a8xx
   guards in that function cover *other* steps, not this one.

So the hypothesis was retired on a wrong premise and never tested.

## Our own tree already flagged it, in image 209

`a6xx_gpu.c:1598-1601`, in the image-209 diagnostic comment:

> "the fault is GPU-side (page table / SMMU translation -- upstream's
> a6xx_submit() emits a prologue we emit nowhere: CP_SET_PROTECTED_MODE=0,
> PERFCTR_SRAM_INIT_CMD, and CP_SMMU_TABLE_UPDATE with TTBR0+ASID)"

It was identified as the discriminator, used to justify a CPU-vs-GPU-side
diagnostic (images 209-213), and then **never implemented**. The diagnostic
answered its question and the follow-up was dropped.

## Why it fits the observed symptom

TTBR0 is the page-table base the GPU's shader/interconnect domain translates
through. If it is never programmed:

- the **shader domain** (RB render stores and SP compute stores) has no valid
  translation, so its stores are discarded
- no fault is necessarily raised — a discard is not a fault
- the **CP's own writes** (the `memptrs->fence` increment that proves CP→DDR
  works) may not traverse the same translation path
- register contents are unaffected, which matches "Mesa's state is live in
  silicon"

That is the exact signature we observe: **registers live and workload-tracking,
no fault, no CP-domain problem, no shader-domain write from any source.**

It is also the only remaining hypothesis that is *shared* between RB and SP and
lives below register programming, which is what the compute result forces.

## The counter-consideration — stated, not buried

Upstream does have code that declines to program some registers because "the
XBL will take care of that" (`a6xx_gpu.c:1993-1997`, for the LLC slice). If the
bootloader already programmed TTBR0, the packet could be unnecessary and this
lead would be wrong.

Against that: if TTBR0 were correctly programmed at boot, shader-domain stores
would work. They do not, in any configuration, ever. So either TTBR0 is wrong /
never set, or it is set and something downstream is. Either way this is the
first hypothesis that engages the actual asymmetry.

**This has not been proven. It is a hypothesis with a verified mechanism and a
verified divergence, nothing more.**

## Implementation cost, if we pursue it

The packet emission itself is straightforward. The real work is obtaining
TTBR0 + ASID in the submit path:

- Upstream uses `msm_iommu_pagetable_params()` — **absent from this tree**
  (no `ttbr0` plumbing anywhere in `drivers/gpu/drm/msm/` except the GMU fault
  snapshot at `a6xx_gmu.c:3651`, which *receives* TTBR0 as an argument)
- So we would need to plumb the page-table base and ASID out of the 4.14 CAF
  MMU/IOMMU attach path into `a6xx_submit()`

That is a real change to the MMU plumbing, not a one-liner — and unlike the
GBIF experiment it cannot be validated by a parameter flag alone. It would need
its own build, its own image, and a careful design that does not disturb the
submit path (264/265/266 lesson).

## Recommended next step

1. **Do not implement yet.** First establish, source-only, *where* TTBR0 lives in
   this CAF tree: whether `msm_mmu`/the SMMU attach already records it, whether
   it can be read back from the GMU SMMU registers the way
   `a6xx_gmu_snapshot_on_smmu_fault()` does, and what ASID the domain is using.
   That is cheap and decides whether the fix is 20 lines or 200.
2. Only then decide whether to build.

## Files

- `upstream-a6xx_gpu.c` — upstream reference, fetched this session
- `patches/` — unchanged; no patch produced for this yet, by design