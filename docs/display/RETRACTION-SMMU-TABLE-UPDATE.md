# RETRACTION: the missing `CP_SMMU_TABLE_UPDATE` is NOT the bug

Date: 2026-11-11. `ROOT-CAUSE-MISSING-SMMU-TABLE-UPDATE.md` is **withdrawn**.
Our port is faithful to v4.19; the omission is correct.

## The finding that settles it

Linux **v4.19 has no `a6xx_set_pagetable()` at all**, and v4.19's `a6xx_submit()`
does not call it. Fetched in full from
`raw.githubusercontent.com/torvalds/linux/v4.19/drivers/gpu/drm/msm/adreno/a6xx_gpu.c`:

```c
static void a6xx_submit(struct msm_gpu *gpu, struct msm_gem_submit *submit,
	struct msm_file_private *ctx)
{
	struct msm_drm_private *priv = gpu->dev->dev_private;
	struct msm_ringbuffer *ring = submit->ring;
	unsigned int i;

	OUT_PKT7(ring, CP_EVENT_WRITE, 1);
	OUT_RING(ring, PC_CCU_INVALIDATE_DEPTH);
	OUT_PKT7(ring, CP_EVENT_WRITE, 1);
	OUT_RING(ring, PC_CCU_INVALIDATE_COLOR);
	... MSM_SUBMIT_CMD_BUF -> CP_INDIRECT_BUFFER_PFE ...
	OUT_PKT4(ring, REG_A6XX_CP_SCRATCH_REG(2), 1);
	OUT_RING(ring, submit->seqno);
	OUT_PKT7(ring, CP_EVENT_WRITE, 4);
	OUT_RING(ring, CACHE_FLUSH_TS | (1 << 31));
	OUT_RING(ring, lower_32_bits(rbmemptr(ring, fence)));
	OUT_RING(ring, upper_32_bits(rbmemptr(ring, fence)));
	OUT_RING(ring, submit->seqno);
	a6xx_flush(gpu, ring);
}
```

**Byte-for-byte the same shape as ours.** I proposed backporting a v5.10-era
mechanism into a v4.19-based port. That was wrong.

## When it was introduced, and why it does not apply

| tag | `set_pagetable` | `CP_SMMU_TABLE_UPDATE` |
|---|---|---|
| v4.19 | 0 | 0 |
| v5.4 / v5.5 / v5.6 / v5.9 | 0 | 0 |
| **v5.10** | 2 | 6 |
| v5.15 / v6.1 / master | 2 | 6 |

Commit `84c31ee16f903b481e7d4b94e2fc931d373494a0`, Jordan Crouse, 2020-09-12,
*"drm/msm/a6xx: Add support for per-instance pagetables"*, merged in v5.10.

**Decisive detail:** `msm_iommu_pagetable_params()` returns `-EINVAL` unless
`mmu->type == MSM_MMU_IOMMU_PAGETABLE`, and that type is only set by
`msm_iommu_pagetable_create()`, which requires Qualcomm's `adreno_smmu`
implementation and `WARN_ONCE(!ttbr1_cfg)` otherwise. So **on current master,
with a plain `arm-smmu-v3` GPU domain, `a6xx_set_pagetable()` emits nothing at
all.** Emitting nothing is a supported, upstream-tested configuration.

`CP_SET_PROTECTED_MODE` and `RBBM_PERFCTR_SRAM_INIT_CMD` are later still —
v5.18, *"drm/msm/a6xx: Zap counters across context switch"* — and also only in
the context-switch path. I had attributed them to the base submit path; they
are not there.

## CAF agrees

In CAF msm-4.14 (A640 branch), `adreno_iommu_set_pt_ctx()` picks:

```c
cpu_path = !(flags & ADRENO_CONTEXT_SWITCH_FORCE_GPU) &&
		_ctx_switch_use_cpu_path(adreno_dev, new_pt, rb);
if (new_pt != cur_pt) {
	if (cpu_path) result = _set_pagetable_cpu(rb, new_pt);
	else           result = _set_pagetable_gpu(rb, new_pt);  /* CP_SMMU_TABLE_UPDATE */
}
```

The **default** path is `_set_pagetable_cpu()` → `kgsl_iommu_set_pt()`, which
writes the arm-smmu context-bank registers from the CPU
(`TTBR0`, `CONTEXTIDR`, `mb()`, `TLBIALL`). `CP_SMMU_TABLE_UPDATE` is only
emitted for **per-process page tables** and preemption. There is no
`RBBM_PERFCTR_SRAM_INIT_CMD` anywhere in CAF a6xx.

Both upstream v4.19 and CAF establish the GPU page table through the **external
arm-smmu**, and the CP issues zero MMU packets. Omitting the packet is correct.

## The symptom argument, which kills it independently

Even setting version history aside, the measurement contradicts it:

> `CP_RB_RPTR` is advanced by the CM3 microcode as it **retires packets it has
> fetched from the ringbuffer**. `RPTR == WPTR` is direct evidence that the
> ringbuffer VA translated correctly *and* that the CM3 was decoding the stream.
> A bad or zero TTBR would stall the very first fetch — `RPTR` would stay put
> and you would get `A6XX_CP_AHB_ERROR` / a fault, not "RPTR chases WPTR".

So `RPTR` advancing to `WPTR` is positive evidence that address translation
works and the CM3 is decoding. That rules the page-table family out on the
evidence, not merely on provenance.

## What this leaves

The signature is now: **the CM3 is fetching and decoding, and something about
what it decodes is wrong.** Surviving candidates:

1. **The ring memory does not contain what the CPU wrote.** This is the
   untested one, and it is what image 211's `RINGCPU` finally measures.
2. **The SQE ucode blob is wrong for an A640.** v4.19's `a6xx_cp_init()` writes
   `CP_ME_INIT` with `0x2f, 0x3, 0x20000000, 0,0, 0,0,0` — which our tree
   matches exactly — but the shipped `a630_sqe.fw` may still be the wrong
   generation for A640. Being checked separately.
3. **The per-submit IRQ is `RBBM_HANG_DETECT`, not `CP_CACHE_FLUSH_TS`.** If
   the interrupt is a hang report, that reframes everything — the CP is
   detecting a hang rather than completing a timestamp. Note v4.19's
   `a6xx_fault_detect_irq()` prints only `ring->seqno`, not `memptrs->fence`.
4. **`CP_SCRATCH_REG2` may be a false signal** if the register is not readable
   on this generation, leaving the fence as the only real evidence.

## Correction to my process

I compared our tree against **master** and read the difference as a missing
feature, without first asking which upstream version the port was based on. The
port is explicitly a v4.19 backport (`donor/v419/`). **Version provenance is
the first question when diffing a backport against upstream**, not the last.
