# STATUS: the failure is fully characterised; one link is unproven

Image 256. Stopping here rather than starting another image, because every
build I have made on an unproven link has turned out to be chasing my own
mistake.

## What is established, on a validated instrument

`BOSXCHK` reads each BO through two independently built mappings — the
`msm_gem_get_vaddr()` view and one built from `bo->sgt->sgl`, the pages the GPU
is actually handed — and they agree **8/8**. The `0x5C5C5C5C` tag write that
corrupted images 250-253 is removed, so contents are reported as they are.

**1. The shader ISA buffer is entirely zero, at a size that tracks the
compiled shader.**

```
bo[1]  obj=...059a00  size=20480   0 nonzero    <- 120-term fragment shader
       (trivial shader: size=6440, also 0 nonzero)
```

Mesa reports `compile ok=1`, `link ok=1`, allocates the right size, and emits
the right address (`fd6_emit_shader()`: `OUT_RELOC(ring, so->bo, ...)` →
`SP_xS_OBJ_START_LO`, and that IOVA is in the stream at the expected place).
The SP is pointed at a correctly sized, correctly addressed, entirely zero
instruction buffer. Executing zeros is a no-op, not a fault — which is why this
has never once produced an interrupt, a GMU error, or a hang-detect trigger.

**2. The buffer is never CPU-mapped, and no mmap fails.**

```
MMAP256: mmap requested obj=(null) pgoff=1057159
MMAP256: drm_gem_mmap OK obj=ffffffc366059800      <- this is bo[2], 4096 B
```

Exactly **one** mmap in the whole test, and it succeeded. The ISA buffer
`bo[1]` (`ffffffc366059a00`, 20480 B) is **never mapped at all** — and no mmap
was rejected, so this is not a silent failure we were missing. (Image 255 said
otherwise because its hook sat after the failure point; image 256 moved it and
corrected that.)

**3. The object is ordinary in kernel terms.** `flags=0x20000` = `MSM_BO_WC`,
a normal writecombine mapping. Nothing in the kernel marks it unmapable.

## The mechanism, with the one gap marked

```
ir3_gallium.c:110   fd_bo_new(dev, size, FD_BO_NOMAP, ...)   <- shader ISA
ir3_gallium.c:116   fd_bo_upload(bo, v->bin, 0, v->info.size);
  └─ char *dst = fd_bo_map(bo);
     if (dst) { memcpy(dst + offset, src, size); }
     ...no else branch, no log
```

`FD_BO_NOMAP` is a **Mesa-internal** flag, invisible to the kernel. The
evidence says `fd_bo_map()` returns NULL for this buffer *without even
attempting an mmap* — which is the only way to get "never mapped, no
failures" rather than "mapped, then zeroed". The memcpy is then skipped in
silence and the buffer stays zero at exactly the right size.

**The gap:** why does `fd_bo_map()` refuse here? And the obvious objection is
that this code is common to all Adreno parts, so it cannot simply be broken on
mainline — `FD_BO_NOMAP` is used the same way for lrz, tessfactor and vsc
streams, none of which are CPU-written, but the shader ISA is the one NOMAP
buffer that is. Something must make `fd_bo_map()` succeed for it in a working
build, and I cannot see what from here.

## What blocks closing it

`fd_bo_map()` lives in `freedreno/util/bo.c`, which is **not in our partial
Mesa extract** and **cannot be fetched** — every source URL tried
(gitlab.freedesktop.org, raw.githubusercontent.com, several tags and paths) is
blocked or returns a 404 stub in this environment. The only working tool is
`websearch`, which returns summaries, not source.

So the honest position: the mechanism is coherent and well-evidenced up to the
point where Mesa decides not to map, and the deciding code is unavailable.

## Options

1. **Get `bo.c` another way** — no kernel build needed, so it costs nothing to
   try first. Ideas: `apt-get source mesa` on the device (if it has network),
   a Debian source mirror over the phone's wifi, or a pip/conda copy of the
   source tarball on the host.
2. **Trace the remaining map paths in the kernel** — `msm_gem_prime_mmap` and
   the dma-buf mmap route are still untraced. If Mesa reaches the buffer by
   either, that changes the conclusion and is worth knowing. One image.
3. **Stop and document.** The root cause is unambiguous enough to act on
   upstream; the remaining link is a userspace detail.

I'd do (1), then (2) only if it fails.

## Correction ledger

Wrong because the *probe* was wrong, not the results:
- "measurement chain reads the wrong memory" (image 251) — false;
  `virt_to_phys()` is invalid on vmalloc addresses.
- "cache coherency / bo_mmap_wc is the cause" — false, built on the above.
- "buffer upload path is broken" — false; `remap.py` showed data surviving.
- "Mesa never mmaps the ISA buffer, so the upload is skipped" (image 255) —
  the *conclusion* happens to be right but the reasoning was wrong; the hook
  was after the failure point, so the trace could not distinguish "never
  attempted" from "attempted and rejected". Image 256 settled it properly.

Root causes built and abandoned: LLC/SCID/slice attributes, clocks, hwcg
tables, secure mode/zap, cache coherency + `bo_mmap_wc`, the GEM252 purge
race, the kernel/GPU aliasing theory, and FD_BO_NOMAP-as-mmap-blocker (still
open, not abandoned).