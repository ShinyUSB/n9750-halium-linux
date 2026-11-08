# CORRECTION: the "userspace writes are invisible to the kernel" result was a NULL TEST

This retracts `ROOT-CAUSE-USERSPACE-WRITES-INVISIBLE.md`, written earlier
today. The conclusion in that document is **not supported by the evidence**.

## What was claimed

`pattern.py` wrote `0xA5A5A5A5` via `glBufferData` and `0x5A5A5A5A` via
`glMapBufferRange`, submitted a draw, and the kernel's BOS dump showed:

```
TOTALS: dwords=12126  A-pattern=0  B-pattern=0
VERDICT: userspace writes are INVISIBLE to the kernel (mmap aliasing bug)
```

That was interpreted as: userspace writes land in physical pages that are not
the GEM object's, so the GPU DMAs from the real (zeroed) pages.

## Why that was wrong

The BOS dump only covers the buffers in the submission's `bos[]` array. Full
re-analysis of the same capture, showing **every** BO rather than only the ones
with nonzero content (the earlier analysis script silently skipped all-zero BOs):

```
bo[ 0] ndwords=1024  nonzero=1
bo[ 1] ndwords=5120  nonzero=0      <- shader ISA, 20480 B
bo[ 2] ndwords=1024  nonzero=0      <- shader ISA, 4096 B
bo[ 3] ndwords=1024  nonzero=72
bo[ 4] ndwords=1024  nonzero=1
bo[ 5] ndwords=1024  nonzero=149
bo[10] dumped=862    nonzero=0
bo[11] ndwords=1024  nonzero=244
```

`pattern.py` sets `NDW = 256`, i.e. each test buffer is 256 dwords = 1024 bytes.
**No BO with `ndwords == 256` exists in the list.**

So neither `bufA` nor `bufB` was ever submitted. The kernel's dump had no
opportunity to observe them, and "A-hits=0" is a statement about which buffers
were submitted, not about memory aliasing. The test could not have produced any
other result.

This is the same class of error as image 249/250 (miscomparing a 32-byte count
against 16), and the same class as the "6/6 MISMATCH" that turned out to be the
`virt_to_phys` probe bug: **a measurement that cannot fail is not evidence.**

## What survives

- The shader ISA buffers (`bo[1]` 20480 B, `bo[2]` 4096 B) genuinely **are**
  all-zero in the kernel's view of its own submission. Those BOs *are* in
  `bos[]`, so this observation is real and still unexplained.
- The subagent review's catch still stands: the "small shader works, large one
  doesn't" reading was an artifact of a single reloc-patched dword.
- The shader-cache hypothesis is **refuted**: with `~/.cache/mesa_shader_cache`
  deleted and `MESA_SHADER_CACHE_DISABLE` + `MESA_GLSL_CACHE_DISABLE` set and a
  cold compile forced, the ISA buffers remain zero.
- `GEM_INFO` (`0xc0186443`) and `GEM_NEW` (`0xc0106442`) are both issued by Mesa
  and both succeed, confirmed by strace. Two `0x8000`-byte `MAP_SHARED` mmaps
  do reach the driver.

## Hypotheses checked and killed while investigating

- **`get_pages_vram()` / `phys_to_page()` on an IOVA.** `physaddr()` returns
  `vram_node->start << PAGE_SHIFT + priv->vram.paddr`, and `get_pages_vram()`
  fabricates `struct page *` via `phys_to_page()`. If `vram.paddr` were a DMA
  IOVA this would be a genuine, severe bug. It is not: no "VRAM carveout"
  probe message appears in dmesg, so `priv->vram.size == 0` and every BO uses
  plain `drm_gem_get_pages()`. Dead code here.
- **Generic DRM `gem_vm_ops` instead of msm's.** `msm_gem_mmap()` does omit
  upstream's `vma->vm_ops = &gem_vm_ops`, which looked like the bug. It is not:
  `drm_gem_mmap_obj()` already installs `dev->driver->gem_vm_ops`, which for msm
  is the ops whose `.fault` is `msm_gem_fault`. Both the fault handler and
  `msm_gem_get_vaddr()` therefore read the same `msm_obj->pages`. The omission
  is a latent upstream-drift issue, not today's bug.

## The methodology rule this establishes

Any harness that infers memory contents from the kernel's `bos[]` dump must
**first prove its buffer under test reached `bos[]`** — by size, by name, or
both. Otherwise it measures nothing and will "confirm" whatever it was looking
for.

Concretely, every future test needs, before any content comparison:

1. the test buffer's size to appear among the submitted BOs, and
2. `MSM_INFO_SET_NAME` implemented, so Mesa's own labels (`"fs:…"`, `"vs:…"`,
   `"pvtmem_per_fiber"`, `"gmem:…"`) identify each buffer authoritatively
   rather than by size inference.