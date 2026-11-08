# THE MEASUREMENT CHAIN WAS BROKEN. Every "empty buffer" result is withdrawn.

Image 251. This is the finding that explains why images 246-250 kept producing
contradictory conclusions: we were reading the wrong physical memory and
calling it data.

## The proof

At submit, for every BO, compare where the kernel's reader points against where
the GPU is actually pointed:

```
BOSPHYS[seq=1] bo[6] vaddr=ffffff801ff46000 virt_to_phys=0x97f46000 sgt_phys=0x3cb3f9000 sgt_nents=8  != MISMATCH
BOSPHYS[seq=1] bo[7] vaddr=ffffff8019e65000 virt_to_phys=0x91e65000 sgt_phys=0x3ca7d9000 sgt_nents=1  != MISMATCH
BOSPHYS[seq=1] bo[8] vaddr=ffffff8019e6d000 virt_to_phys=0x91e6d000 sgt_phys=0x3ca7b1000 sgt_nents=1  != MISMATCH
BOSPHYS[seq=1] bo[9] vaddr=ffffff8019e75000 virt_to_phys=0x91e75000 sgt_phys=0x3ca7da000 sgt_nents=1  != MISMATCH
BOSPHYS[seq=1] bo[10] vaddr=ffffff8019e7d000 virt_to_phys=0x91e7d000 sgt_phys=0x3e7aec000 sgt_nents=1  != MISMATCH
BOSPHYS[seq=1] bo[11] vaddr=ffffff8019dad000 virt_to_phys=0x91dad000 sgt_phys=0x3ca7dc000 sgt_nents=1  != MISMATCH
```

**6 of 6 mismatch**, and not marginally: `virt_to_phys` lands in a contiguous
`0x9xxxxxxx` band while the real buffer memory is at `0x3c…`. These are
different pages entirely, not a stale-cache variant of the right ones.

`msm_gem_get_vaddr()` — which every "the buffer is all zeros" result in this
project came from — returns a mapping of the **wrong physical memory**.

## Consequence: four conclusions are withdrawn

| withdrawn claim | based on |
|---|---|
| shader ISA buffer is empty | `BOS[]` dump, wrong memory |
| userspace writes never reach the kernel | same |
| cache coherency / writecombine as the cause | built on the above |
| image 249 `bo_mmap_wc` as a fix | built on the above (and measurably did nothing) |

Only the `vaddr`/`ndw`/buffer-graph parts of those dumps are still meaningful —
the *structure* (which BO is referenced from where, IOVAs, sizes) came from
metadata, not contents. That is why the "bo[1] scales with shader size" finding
survived and looked convincing: the size is real, the **contents are not**.

## What is still solid

Everything not derived from MMIO contents:

- CP domain healthy: ring drains, `CP_TIMESTAMP clocking 1`,
  `CP_INTERRUPT_STATUS` always 0, no GMU error.
- Clocks: 43/46 hwcg entries read back exactly as CAF's a640 table specifies,
  zero `0xdeadbead`.
- zap firmware loads and runs, and compute was still dead with it loaded.
- `pm_hold` works: `active +=25006 ms` over 25 s, `suspended +=0`.
- `SPTPRAC_PWR_CLK_STATUS=0`, `RPMH_POWER_STATE=0` — the GPU is fully powered
  even while `runtime_status` reads `suspending`.
- glReadPixels returns a sound zero (Phase-0 oracle proved the read path).
- The command-stream BOs showed plausible command data — but note that was
  also read through the broken reader, so even that is now unverified.

## The fix

Read the dump through the SGT — the same pages the GPU is given. Concretely, in
the `BOS[]` dump, map `bo->sgt->sgl` pages (e.g. `sg_page(sgl)` +
`sg_phys(sgl)`) instead of `msm_gem_get_vaddr()`, or use
`dma_buf_begin_cpu_access()` on the object. Then re-run.

**Do not draw any conclusion about buffer contents before the
`BOSPHYS` line says `== ALIASES`.** That check should stay in the dump
permanently as a guard: a diagnostic that silently reads the wrong memory is
worse than no diagnostic, and this project spent several images on it.

## Suspicion worth noting

`madv=0` on every BO. `msm_gem_get_vaddr()` guards on
`msm_obj->madv != MSM_MADV_WILLNEED` and would return `-EBUSY` otherwise, so 0
appears to be the accepted value here. But given the address mismatch, it is
worth checking whether `msm_obj->vaddr` is a **stale mapping from an earlier
object state** — e.g. left over across a purge/realloc — rather than a fresh
map of the current pages. `get_pages()` populates `msm_obj->pages` and
`msm_obj->sgt` from the same array, so they should agree; they evidently do not
in practice.

## Honest accounting

Four confident theories were built and then withdrawn — empty shader ISA, lost
upload, cache incoherency, and the writecombine fix. Two of those failures were
my own analysis bugs (a byte/dword comparison, and a count compared against the
wrong expected value). The underlying common cause is now identified: **we were
reading the wrong physical memory the whole time.** That is worth more than any
of the theories, because it means the next measurement will be the first
trustworthy one.