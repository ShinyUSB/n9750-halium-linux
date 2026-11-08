# Root cause: the a6xx port never programs the GPU's page table

Date: 2026-11-11. Web research (upstream msm + CAF KGSL) against the local tree.
**This supersedes `FOUND-MEMPTRS-IOVA-ZERO.md` as the primary explanation.**

## The finding

Upstream `a6xx_submit()` begins with `a6xx_set_pagetable()`, which for a6xx
emits three things our port never emits anywhere:

```c
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
```

Measured against this tree:

| symbol | opcode defined? | used in driver? |
|---|---|---|
| `CP_SMMU_TABLE_UPDATE` (= 83) | **yes**, `adreno_pm4.xml.h:208` | **no — 0 occurrences in any .c** |
| `CP_SET_PROTECTED_MODE` (= 95) | yes, `adreno_pm4.xml.h:179` | only in `a5xx_power.c`, **not in the a6xx submit path** |
| `REG_A6XX_RBBM_PERFCTR_SRAM_INIT_CMD` | **no — not defined at all** | no |
| `set_pagetable` | — | **0 occurrences** |
| `CP_ALWAYS_ON_CONTEXT` | — | **0 occurrences** |
| `ttbr` | — | only in a GMU snapshot *read* (`a6xx_gmu.c:3107`) |

`a6xx_cp_init()` emits only `CP_ME_INIT`. There is no page-table programming
anywhere in the a6xx path — not in bring-up, not in submit.

**The opcode that fixes this has been sitting in our own header, unused, for
the entire project.**

## Why this produces exactly the observed signature

```
before submit   CP_RB_WPTR 0x63  CP_RB_RPTR 0x63   IRQ 487 = 0
after  submit   CP_RB_WPTR 0x72  CP_RB_RPTR 0x72   IRQ 487 = 1
CP_SCRATCH_REG2: 0        fence: 0/1        RB_RPTR_ADDR = 0x0
```

The ring itself is fetched through the always-on/fixed path, so the CP walks it
and `RPTR` correctly chases `WPTR` — the GPU is demonstrably fetching. But every
address the *command stream* touches requires a translated page table, and there
isn't one. So:

- the `CACHE_FLUSH_TS` write to `rbmemptr(ring, fence)` never lands → `fence` stuck at 0
- `CP_SCRATCH_REG(2)` never gets written → reads 0
- the hangcheck correctly declares a lockup on every tick
- the a6xx IRQ still fires (RBBM_INT_0), because that path does not need the page table

A GPU that fetches but cannot translate is precisely "RPTR chases WPTR, nothing
happens". This is a much better fit than anything previously proposed, because it
explains the fetching *and* the non-execution with one cause.

## Hypotheses eliminated by this research

**"Missing doorbell / `RB_MASTER_CONTROL` / `FORCE_PUSH`" — DISPROVED.**
Fetched CAF KGSL `adreno_a6xx.c` and `adreno_a6xx_preempt.c` (2711 and 809
lines). The only ring-kick write in either file is `CP_RB_WPTR`. There is no
`MASTER_CONTROL` or `FORCE_PUSH` anywhere in KGSL, and KGSL works on this exact
hardware. Our `a6xx_flush()` is structurally identical to the working driver.

**"`CP_RB_CNTL` is wrong" — DISPROVED.** Upstream
`MSM_GPU_RB_CNTL_DEFAULT = BUFSZ(ilog2(32K/8)) | BLKSZ(ilog2(32/8))` = `0x20c`,
which is exactly our measured value. Upstream only *adds*
`AXXX_CP_RB_CNTL_NO_UPDATE` (0x08000000) for targets without extended APRIV.
Worth revisiting, but not the bug.

**"`CACHE_FLUSH_TS` is mis-encoded" — DISPROVED.** Upstream writes
`CP_EVENT_WRITE_0_EVENT(CACHE_FLUSH_TS) | CP_EVENT_WRITE_0_IRQ`, where
`CP_EVENT_WRITE_0_EVENT(x)` is `(x << 0) & 0xff`, so `CACHE_FLUSH_TS` (= 4)
stays 4 and the IRQ bit is 0x80000000. Ours writes `CACHE_FLUSH_TS | (1 << 31)`
= `4 | 0x80000000`. Identical.

**"Ring/memptrs IOVA collides with the GMU firmware window" — probably a red
herring.** `memptrs_iova == 0` and `rb_iova == 0x1000` do fall inside the GMU's
fixed `itcmhole` window (`iova=0x0 size=0x4000`), but the GMU uses its own
`gmu->domain` with SID `0x4000005`, the GPU's stream IDs are in the `0x20xx`
range, no SMMU faults were reported, and the CM3 keeps running. Separate context
banks is by far the most likely reading. `memptrs_iova == 0` remains a real oddity
worth printing, but it is not required to explain the symptom.

## Honest confidence

**High** that the missing submit prologue is a genuine, large gap — it is
verified absent, the opcode exists unused in our own header, and upstream emits
it on every submit.

**Medium** that it is *the* bug. The chain depends on this platform's GPU MMU
configuration. CAF may run the GPU behind the arm-smmu in a mode where the
hardware page tables are shared and `CP_SMMU_TABLE_UPDATE` is unnecessary — in
which case its absence is benign and the real fault is still ahead of us.

I cannot settle that from the source alone; it needs a measurement.

## Implementation obstacles (already identified)

1. The `CP_SMMU_TABLE_UPDATE_0/1/2/3` **field macros do not exist** in
   `adreno_pm4.xml.h` — only the opcode enum value 83. They must be added:
   `TTBR0_LO` (bits 0-31), `TTBR0_HI` + `ASID`, `CONTEXTIDR`, `CONTEXTBANK`.
2. `REG_A6XX_RBBM_PERFCTR_SRAM_INIT_CMD` is **not defined anywhere**.
3. There is no `msm_iommu_pagetable_params()` equivalent — this CAF tree has
   `msm_gpu_mmu.c` instead of upstream's `msm_iommu.c`, and the ttbr0/asid
   plumbing does not obviously exist yet.

## Recommended next step

One **measurement-only** image that changes no behaviour and answers the
question that decides everything — whether this GPU configuration needs the
page-table update at all:

- emit upstream's exact prologue (`CP_SET_PROTECTED_MODE`=0,
  `PERFCTR_SRAM_INIT_CMD`=1, `CP_SMMU_TABLE_UPDATE`) using the real ttbr0/asid
  for the GPU aspace, if they can be obtained; otherwise
- at minimum, print the GPU aspace's page-table base and ASID, plus a dump of
  the first 16 ring dwords from the CPU side after `a6xx_submit()`, and
  `memptrs->fence` / `memptrs->rptr` at the same instant

Outcomes:

| observation | conclusion |
|---|---|
| ring dwords contain valid packet headers, fence still stuck | CPU side is correct; GPU-side translation/page table is the gap — proceed with the upstream prologue |
| adding the prologue makes `fence` advance and `glstage 6` return pixels | **goal reached — real GPU producing pixels** |
| ring dwords are garbage or stale | ring memory coherency is real after all, revisit the mapping question |

The `GL_RENDERER = FD640` gate applies to every one of these runs. A stage result
on `llvmpipe` means nothing, and that has already cost two images.
