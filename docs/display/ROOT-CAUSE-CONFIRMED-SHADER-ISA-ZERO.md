# ROOT CAUSE CONFIRMED: the shader ISA buffer submitted to the GPU is entirely zero

Measured with a **validated** instrument. This supersedes the withdrawal in
`CORRECTION-CACHE-COHERENCY.md` / `CORRECTION-BOUPLOAD-WORKS.md`, which were
themselves based on a broken probe.

## The instrument is now trustworthy

Image 254, two independent checks:

1. `BOSXCHK` builds a second mapping directly from `bo->sgt->sgl` — the pages
   the GPU is actually handed — and compares its contents against the
   `msm_gem_get_vaddr()` view. Result: **IDENTICAL CONTENT for 8/8 BOs**.

   So the `BOS[]` dump has been reading real buffer memory all along.

2. The `0x5C5C5C5C` tag write is gone (`BOSALIAS: 0`). It existed only to prove
   aliasing from userspace, and while live it corrupted every measurement —
   BOs came back with exactly 16 nonzero dwords, which was the tag, not data.

## What image 251's "mismatch" actually was

`virt_to_phys()` is **invalid on vmalloc-range addresses** on arm64
(`__pa()`/`vmalloc_to_phys()` is correct). `vmap()` returns exactly such an
address, so `virt_to_phys=0x9xxxxxxx` was garbage, and comparing it against
`sgt_phys` produced a false `6/6 MISMATCH`. The probe was wrong; the driver and
the original dump were fine.

`BOSPHYS` no longer prints a verdict, and labels the value `(UNRELIABLE)`.

## The measurement

120-term fragment shader, so the compiled ISA is large and unmistakable:

```
bo    iova           dwords   nonzero
0     0x700022000      1024         1
1     0x700025000      5120         0     <-- SHADER ISA
2     0x700038000      1024         0
3     0x70002b000      1024        72     <-- command stream
4     0x700012000      1024         1
5     0x70002c000      1024       149     <-- command stream
11    0x70002a000      1024       244     <-- command stream
```

`bo[1]` is the ISA buffer, established by correlation: a trivial shader gives
1610 dwords, the 120-term shader gives 5120 — it tracks the compiled ISA size.
**It is zero, in full, at both sizes.**

Meanwhile Mesa reports `compile ok=1` / `link ok=1`, allocates the correct size,
and emits the correct GPU address for it: `fd6_emit_shader()` writes
`OUT_RELOC(ring, so->bo, ...)` as `SP_xS_OBJ_START_LO`, and that IOVA is present
in the command stream at the expected position.

So: the shader processor is pointed at a correctly sized, correctly addressed,
**entirely zero** instruction buffer. Executing zeros is a no-op, not a fault —
which is precisely why there has never been an interrupt, a GMU error, a
hang-detect trigger, or any diagnostic output of any kind.

## Why it also explains the clear

A tile clear runs on the RB/CCU with no shader involvement, so an empty ISA
alone cannot explain `glClear` returning `[0,0,0,0]`. But a single upstream
fault reaches the render target's contents too — whatever zeroes the ISA also
zeros the FB. That is now the single fault to explain, rather than two.

## What is ruled out

| candidate | status |
|---|---|
| unclocked RB/CCU/UCHE | refuted — 43/46 hwcg entries match CAF exactly, no `0xdeadbead` |
| wrong hwcg addresses | refuted — zero unmapped reads across all 46 |
| secure mode / missing zap | refuted — zap loads, runs, and compute is still dead |
| render-vs-handoff split | resolved — render itself is broken |
| read path broken | refuted — `glReadPixels` returns a sound zero |
| buffer upload path lost | **refuted** — two independent mappings both see the data |
| kernel mapping not aliasing GPU memory | **refuted** — 8/8 identical content |
| shrinker purging the buffer | refuted — `madv=0` (WILLNEED), and `is_purgeable()` requires DONTNEED |
| stale `vaddr` (GEM252) | real latent bug, but `GEM252: 0` — it never fired here |

## Remaining lead

Mesa writes the ISA via:

```c
v->bo = fd_bo_new(compiler->dev, v->info.size, FD_BO_NOMAP, ...);
fd_bo_upload(v->bo, v->bin, 0, v->info.size);
```

and `fd_bo_upload` is a silent no-op if its map fails:

```c
char *dst = fd_bo_map(bo);
if (dst) { memcpy(dst + offset, src, size); fd_bo_unmap(bo); }
/* no else, no log */
```

`FD_BO_NOMAP` marks GPU-only buffers elsewhere (lrz, tessfactor, vsc streams);
the shader ISA BO is created the same way and then CPU-written. We have proven
the *kernel* can map that BO, so the remaining suspects are on the Mesa side:
whether `fd_bo_map()` returns NULL for a NOMAP buffer in this Mesa build, or
whether the `ir3` shader cache is handing back an empty `bin` on a cache hit.

Next step is on the Mesa/loader side, not the kernel. `fd_bo_map` is not in our
partial Mesa extract, so either fetch `freedreno/util/bo.c` for 22.3.6 or
instrument the upload path directly.

## Correction ledger (for honesty)

Withdrawn because the *probe* was wrong, not the results:
- "measurement chain was broken / kernel reads wrong memory" (image 251) —
  false; caused by `virt_to_phys()` on a vmalloc address.
- "cache coherency is the cause" — false; based on the above.
- "buffer upload path is broken" — false; `remap.py` showed data surviving, and
  that part was correct.

Reinstated: the empty shader ISA buffer, and userspace patterns being absent
from submitted buffers.

Root-cause candidates built and abandoned along the way: LLC/SCID/slice
attributes, clocks, hwcg tables, secure mode/zap, cache coherency +
`bo_mmap_wc`, the GEM252 purge race, and the aliasing theory.