# Image 206 — kernel GEM mappings were write-combined; the GPU read stale commands

Date: 2026-11-11. Built, packaged, **not yet flashed**. One line of real change.

## Image 205 result: the measurement image did its job

205 (recover no longer power-cycles the GPU) worked exactly as intended. The
instrument survived a full test cycle and gave the decisive answer.

**Baseline, before any GL:**

```
CP_RB_WPTR 0x00000063   CP_RB_RPTR 0x00000063
IRQ 487 (A640) = 0
```

**After one `glstage.py 5` run:**

```
CP_RB_WPTR 0x00000072   CP_RB_RPTR 0x00000072
IRQ 487 (A640) = 1
HANGCHK141: enter fence=0 stored=0 submitted=1
```

**The GPU consumed the entire ring and executed nothing.**

`RPTR` advanced all the way to `WPTR` — the GPU fetched every dword we wrote —
and still produced no result:

- `CP_SCRATCH_REG2: 0`, but `a6xx_submit()` writes `submit->seqno` into that
  register immediately before its `CACHE_FLUSH_TS`
- `fence: 0/1` — memptrs never advanced
- hangcheck correctly declared a lockup

`RB_BASE=0x1000` matches the ring's own iova, so the GPU is reading the right
*address*. Only the *data* is wrong.

That signature — pointer chases correctly, payload is stale — is a coherency
fault, and this project has already met exactly one of those.

## The bug

`msm_gem_get_vaddr()` (`msm_gem.c:717`) hardcoded:

```c
msm_obj->vaddr = vmap(pages, obj->size >> PAGE_SHIFT,
	VM_MAP, pgprot_writecombine(PAGE_KERNEL));
```

`MSM_BO_WC` / `MSM_BO_UNCACHED` were never consulted here — they are only read
in `msm_gem_obj_mmap()`, the **userspace** mmap path. So every *kernel*-mapped
GEM object on this platform was write-combined, including the command ring
itself (`msm_ringbuffer.c:43` allocates it `MSM_BO_WC`).

`a6xx_flush()` cannot compensate. Its `mb()` is `dmb sy`, which orders the
CPU's own accesses; it does not drain a write-combining buffer to the point of
coherency visible to an SMMU-mapped bus master. Upstream v419 is byte-for-byte
identical here — correct on a coherent IOMMU, wrong on this one.

## Same disease as image 202

Image 202 found the identical mechanism on the GMU side: firmware blocks
written through a WC `vmap` were invisible to the CM3, so `INITRES` never became
`0xBABEFACE`, while the mapping and permissions were perfectly valid and
arm-smmu reported **zero** faults. Only the data was wrong. That fix worked by
switching to a coherent allocation.

Image 202 fixed it for the GMU. Nobody checked whether the same hardcoded
mapping was being used for everything else. It was — including the ring.

## The fix

```c
-	VM_MAP, pgprot_writecombine(PAGE_KERNEL));
+	VM_MAP, pgprot_noncached(PAGE_KERNEL));
```

`pgprot_noncached()` is Normal Non-cacheable: coherent with the GPU, still
ordered. Costs some write throughput versus WC, which is the right trade.

This also covers the fence buffer — `adreno_gpu->memptrs` is kernel-mapped via
the same helper (`adreno_gpu.c:595`) — so the CPU can now observe the fence the
GPU writes. That resolves the secondary candidate flagged in
`IMAGE-205-COMING.md` as a consequence of this fix, not as a separate change.

## Decision rule, declared BEFORE flashing

| observation | conclusion |
|---|---|
| `CP_SCRATCH_REG2` ≠ 0 after a submit | **the GPU is executing real commands** — coherency was the last blocker |
| `fence` advances 0 → 1, hangcheck stops firing | fences retire; the deadlock-and-storm class is fully closed |
| `glstage 6` (`glReadPixels`) returns correct pixels | **goal reached — a real GPU producing pixels** |
| `RPTR` still chases `WPTR` with `CP_SCRATCH_REG2` still 0 | coherency was not it; the ring data is being written somewhere else entirely, and the next place to look is `get_wptr()` / where `ring->start` is iova-mapped |

## Verification

- Build exit 0, no new warnings.
- No `pgprot_writecombine(PAGE_KERNEL)` remains in the driver outside comments.
- Byte-identical to 205 in header (all 10 fields), cmdline, ramdisk, size.

```
artifacts/flash/206-halium-boot-a6xx-coherent-ring.img
sha256 dcd5c2087eea87224735fdeaca9d2cdfd5524e46e013da5d57aecaed6e16ea3b
```

## Scoreboard so far

| image | fix | result |
|---|---|---|
| 202 | GMU firmware coherency | bring-up 6-for-6, first reliable GPU |
| 203 | missing `a6xx_last_fence` NULL deref | oops eliminated |
| 204 | recover-path `pm_runtime` deadlock | render node survives, glstage 5/5 |
| 205 | recover no longer power-cycles GPU | instrument survived; GPU proven to consume the ring |
| **206** | **kernel GEM mappings coherent** | **expect real command execution** |

## Not changed

Images 187–205 untouched. 202, 203, 204, 205 all proven and stay.
