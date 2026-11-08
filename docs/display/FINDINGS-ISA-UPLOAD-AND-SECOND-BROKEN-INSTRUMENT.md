# Findings: the shader-ISA upload anomaly, and a second broken instrument

Session log of what was actually established, including two corrections to my
own earlier claims.

## Correction 1 — "userspace writes are invisible" was a NULL TEST

See `CORRECTION-PATTERN-TEST-WAS-A-NULL-TEST.md` and the retraction banner on
`ROOT-CAUSE-USERSPACE-WRITES-INVISIBLE.md`.

`pattern.py` writes 256-dword buffers (`NDW = 256`). The kernel BOS dump only
covers BOs present in the submission's `bos[]` array. Full re-analysis showing
*every* BO (the earlier script had silently skipped all-zero ones) confirms
**no BO with `ndwords == 256` was ever submitted**:

```
bo[ 0] ndwords=1024 nonzero=1     bo[ 5] ndwords=1024 nonzero=149
bo[ 1] ndwords=5120 nonzero=0     bo[10] dumped=862   nonzero=0
bo[ 2] ndwords=1024 nonzero=0     bo[11] ndwords=1024 nonzero=244
bo[ 3] ndwords=1024 nonzero=72
bo[ 4] ndwords=1024 nonzero=1
```

The kernel had nothing to observe. The test could not have returned any other
result.

## Correction 2 — the BOS dump prints the WRONG flags field

This invalidates the cache-mode reasoning I was building at the end of the
session, including any past "flipping the writecombine flag changed nothing"
measurement.

`a6xx_gpu.c:635` prints:

```c
pr_info("BOS[seq=%u] bo[%u] iova=0x%llx flags=0x%x bo=%px\n",
        ..., submit->bos[b].flags, ...);
```

`submit->bos[b].flags` is **internal CAF submit bookkeeping**, not the BO's cache
mode. This tree predates `struct drm_msm_gem_bo` (which does not exist anywhere
in it) and uses the legacy `msm_submit_rel` interface — `msm_gem_submit.c:121`
sets `submit->bos[i].iova = submit_bo.presumed` and the same field carries
`BO_VALID | BO_LOCKED | BO_PINNED | MSM_SUBMIT_BO_WRITE`.

Observed `0x6003` decodes exactly as `BO_VALID|BO_LOCKED|<write>`, nothing to do
with `MSM_BO_WC`. The real cachiness lives in `msm_obj->flags`, which the dump
never printed. So the `MSM_BO_CACHE_MASK` correlation was never actually
tested.

## What is established

1. **The Mesa upload path is exercised, not skipped.** strace shows Mesa issuing
   both `DRM_MSM_GEM_NEW` (`0xc0106442`) and `DRM_IOCTL_MSM_GEM_INFO`
   (`0xc0186443`) with return 0, plus two `0x8000`-byte `MAP_SHARED` mmaps on the
   render fd. `GEM_INFO` works post-UAPI-fix.
2. **`fd_bo_upload` does not silently skip.** Verified in Mesa 22.3.6 source:
   ```c
   memcpy((uint8_t *)bo_map(bo) + off, src, len);   /* no NULL check */
   ```
   A NULL would segfault. `bo_map` returns NULL *silently* only if
   `bo->funcs->offset()` fails, and `ERROR_MSG("mmap failed")` covers the other
   case — neither is observed. So the ISA memcpy executes.
3. **The msm backend has no `.upload` func** (`msm_bo.c:156` sets only
   `.offset`), so `fd_bo_upload` takes the memcpy-via-`bo_map` path as expected.
   `FD_BO_NOMAP` is enforced only in `fd_bo_map()`, which `fd_bo_upload`
   bypasses.
4. **The buffer we identified is the right one.** `fd6_program.c:132` does
   `OUT_RELOC(ring, so->bo, ...)` where `so` is `struct ir3_shader_variant *`,
   so the reloc targets `v->bo` created at `ir3_gallium.c:110` — the ISA BO
   itself, uploaded at `v->info.size`.
5. **The shader cache is not the cause.** With
   `~/.cache/mesa_shader_cache` deleted and `MESA_SHADER_CACHE_DISABLE` +
   `MESA_GLSL_CACHE_DISABLE` set and a cold compile forced, the ISA BOs remain
   zero.

## Hypotheses killed while investigating

- **`get_pages_vram()` / `phys_to_page()` on an IOVA.** `physaddr()` returns
  `vram_node->start << PAGE_SHIFT + priv->vram.paddr` and `get_pages_vram()`
  fabricates `struct page *` with `phys_to_page()`. Genuinely dangerous *if*
  `vram.paddr` were a DMA IOVA. It is not: no "VRAM carveout" probe message in
  dmesg, so `priv->vram.size == 0` and every BO uses plain
  `drm_gem_get_pages()`. Dead code on this configuration.
- **Generic DRM `gem_vm_ops` instead of msm's.** `msm_gem_mmap()` omits upstream's
  `vma->vm_ops = &gem_vm_ops`, which looked like the bug. It is not:
  `drm_gem_mmap_obj()` already installs `dev->driver->gem_vm_ops`, which for msm
  has `.fault = msm_gem_fault`. Both that and `msm_gem_get_vaddr()` read the
  same `msm_obj->pages`. Latent upstream drift, not today's bug.
- **Cache mode as the discriminator** — untestable until Correction 2 is fixed.

## The one structural asymmetry still standing

| BO | created by | alloc flags | content |
|---|---|---|---|
| bo[3], bo[5], bo[11] | kernel a6xx ring | `FD_BO_GPUREADONLY \| FD_BO_CACHED_COHERENT` | data (72/149/244) |
| bo[1], bo[2] shader ISA | Mesa | `FD_BO_NOMAP` | **empty** |

freedreno marks exactly three things `FD_BO_NOMAP` — shader ISA
(`ir3_gallium.c:110`), pvtmem (`fd6_program.c:118`), tess factors
(`fd6_program.c:1313`). In `msm_bo.c:181`:

```c
if (flags & FD_BO_CACHED_COHERENT)
    req.flags |= MSM_BO_CACHED_COHERENT;
else
    req.flags |= MSM_BO_WC;
```

So **every GPU-only buffer is writecombine and every GPU-only buffer is empty**;
every CPU-visible buffer is cached-coherent and has data. This is currently the
best available lead — but it is **untested**, because of Correction 2.

## Proposed next build (not yet built or flashed)

1. Print `msm_obj->flags` (real cachiness) in the BOS dump instead of
   `submit->bos[b].flags`, alongside the existing obj pointer. One line. This is
   a precondition for testing the lead above rather than assuming it.
2. Implement `MSM_INFO_SET_NAME` so Mesa's own labels (`"fs:…"`, `"vs:…"`,
   `"pvtmem_per_fiber"`, `"gmem:…"`, `"cmdstream"`) identify every buffer
   authoritatively, replacing all size-based inference.
3. Re-measure the flags↔content correlation.

Confirm vs refute:
- ISA BOs are `MSM_BO_WC` and command streams are `MSM_BO_CACHED_COHERENT`
  → lead confirmed, proceed to force cached mapping for WC BOs.
- Both are the same cache mode → lead dead, look elsewhere.