# RETRACTION: image 206's coherency hypothesis was wrong, and 208's reasoning was worse

Date: 2026-11-11. Images 206, 207 and 208 are **retracted**. The source tree is
reverted to image 205 state. **Re-flash 205.**

## The core error

This is a 4.14 arm64 tree, and the two macros are named *opposite* to mainline:

```c
/* Kernel/arch/arm64/include/asm/pgtable.h:400 */
#define pgprot_noncached(prot) \
	__pgprot_modify(prot, PTE_ATTRINDX_MASK, PTE_ATTRINDX(MT_DEVICE_nGnRnE) | ...)
#define pgprot_writecombine(prot) \
	__pgprot_modify(prot, PTE_ATTRINDX_MASK, PTE_ATTRINDX(MT_NORMAL_NC) | ...)
```

| macro | actual memory type | coherent with GPU? |
|---|---|---|
| `pgprot_writecombine` | **MT_NORMAL_NC** (Normal Non-cacheable) | **yes** |
| `pgprot_noncached` | **MT_DEVICE_nGnRnE** (Device memory) | **no** |

So the hardcoded `pgprot_writecombine(PAGE_KERNEL)` in `msm_gem_get_vaddr()`
was **already producing coherent Normal Non-cacheable memory**. Image 206
replaced that with Device memory — strictly *less* coherent, exactly backwards.

I asserted the opposite without reading the macro definition in this tree. That
is the single mistake that produced 206, 207 and 208.

## What the evidence actually showed, image by image

### 205 (baseline, keep) — `GL_RENDERER = FD640`, auto-dumps present

The real, unexplained observation:

```
before submit   CP_RB_WPTR 0x63  CP_RB_RPTR 0x63   IRQ 487 = 0
after  submit   CP_RB_WPTR 0x72  CP_RB_RPTR 0x72   IRQ 487 = 1
CP_SCRATCH_REG2: 0        fence: 0/1
```

The GPU advanced `RPTR` to `WPTR` — it consumed the whole ring — and executed
nothing. `RB_BASE=0x1000` matches the ring's own iova, so the address is right;
only the payload is stale.

I attributed this to write-combining. **That attribution is wrong.** The mapping
was Normal-NC and coherent, and `a6xx_flush()`'s `mb()` is sufficient for it.
The stale-payload question is still **open** and still unexplained.

### 206 — REGRESSION. GPU absent, `GL_RENDERER = llvmpipe`

The `pgprot_noncached` change. Bring-up ran (`BABEFACE`, 7/7 ACKs) and then the
`a6xx-load` thread vanished. Mesa fell back to software and every "PASS" was
hollow.

### 207 — REGRESSION, worse. `needs_hw_init == 0`

Removed both `pm_runtime` calls. `gpu->needs_hw_init = true` is set only in
`msm_gpu_pm_runtime_resume()` (`msm_gpu.c:176`) and `a6xx_pm_resume()`
(`a6xx_gpu.c:1539`) — both runtime-PM *resume* paths. So the `get_sync` is the
only thing that **arms** hardware init. Removing it stopped `hw_init` running
at all (`msm_gpu_hw_init no-op (needs_hw_init==0)`, all-zero `CPDUMP`).

This one finding is real and worth keeping: it establishes that the
`pm_runtime_get_sync()` in `adreno_load_gpu()` is **load-bearing**, not
incidental overhead.

### 208 — REGRESSION. The oops, finally visible

```
Unable to handle kernel paging request at virtual address ffffff801fd36004
x1 : ffffff801fd2d010   x0 : ffffff801fd36000
pc : __memcpy    lr : adreno_fw_create_bo+0x64
Call trace: __memcpy -> a6xx_hw_init+0xdf8 -> msm_gpu_hw_init -> adreno_load_gpu
Process a6xx-load (pid: 1165)
```

`dest = ffffff801fd36000` sits inside the GPU MMIO ioremap region, so
`msm_gem_kernel_new_locked()` returned a garbage pointer that passed the
`IS_ERR()` check, and `adreno_fw_create_bo()`'s `memcpy` faulted. This is a
direct consequence of the 206 mapping change (205 completed ucode init fine).

**This also corrects the 206 post-mortem.** I concluded the thread "vanished
into a silent wedge" at `pm_runtime_put_sync`. It did not — **it crashed with
this oops.** There was never a silent wedge in the load path; the thread was
dying and taking `priv->gpu` with it, which is why Mesa saw no GPU.

## Retractions

- `IMAGE-206-COMING.md` — hypothesis wrong (inverted macro names).
- `IMAGE-207-COMING.md` — fix wrong (removed a load-bearing call).
- `IMAGE-208-COMING.md` — reasoning wrong (`put` was never the wedge; the oops was).
- **The "stale ring payload = write-combining" explanation for image 205's
  behaviour is withdrawn.** It remains unexplained.

## Kept and proven

| image | fix | status |
|---|---|---|
| 202 | GMU firmware coherency (`dma_alloc_attrs`, not WC `vmap`) | proven 6-for-6 |
| 203 | missing `a6xx_last_fence` NULL deref | proven, 0 oops |
| 204 | recover-path `pm_runtime` deadlock under `struct_mutex` | proven, render node survives |
| 205 | `a6xx_recover` no longer power-cycles the GPU | proven, instrument survives |

Note that 202's fix is unaffected by this retraction — it did not rely on
`pgprot_*` at all, it changed the *allocation* to `dma_alloc_attrs`.

## Where things stand

The GPU has been brought up reliably since 202 and Mesa has been submitting
since 205. The one open question is the 205 observation: the GPU consumes the
entire ring and produces no effect — no `CP_SCRATCH_REG2` write, fence stuck at
0, hangcheck declares a lockup.

Since the mapping was coherent all along, the remaining candidates are:

1. `get_wptr()` computing a WPTR the GPU interprets differently from the CPU's
   `ring->next` (e.g. ring wrap or a size mismatch between the two).
2. The `CACHE_FLUSH_TS` payload (the `rbmemptr(ring, fence)` iova) not matching
   where the CPU reads `ring->memptrs->fence`.
3. A missing doorbell: `a6xx_flush()` writes only `CP_RB_WPTR`. Some a6xx paths
   need `RB_MASTER_CONTROL`/`FORCE_PUSH` or a `CP_RB_CNTL` enable that this
   tree's `hw_init` may not perform.
4. `CP_SCRATCH_REG2` reads back 0 legitimately (register not readable while the
   CP is running), making it a false signal — the fence would then be the only
   real evidence, and candidate 2 would be the live one.

Next move: re-establish the 205 baseline, then dump the ring buffer contents and
the fence memptrs *from the CPU side* to see whether the commands and the
fence address the GPU was given are even the ones the GPU was pointed at.

## Process lessons

- **Check the macro, not the name.** `pgprot_writecombine` meaning
  "Normal Non-cacheable" is the kind of thing that has to be read in-tree.
  Assuming mainline semantics is what produced three bad images.
- **A crash is not a wedge.** "Thread vanished, no output" was read as a
  deadlock. It was an oops the whole time, one `dmesg` scroll away.
- **Back up before editing, not after.** The `*.bak` copies for 206 and 208
  were taken *after* the edits, so they were useless for revert and both files
  had to be repaired by hand.
