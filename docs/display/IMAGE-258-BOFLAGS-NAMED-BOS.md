# Image 258 — boflags: print the real cachiness, name every BO

```
artifacts/flash/258-halium-boot-a6xx-boflags.img
sha256 999fb8eb8828136bf265c13bcc5b4baa1eadf916e3192a8bfc588f5ff4360d91
```

Build clean, zero warnings in `msm/`. Header diffed against the 242 reference:
23 bytes differing, all inside the sha1 `id` field (576–608), so the only
functional change from 242 is the kernel.

## What changed

Three things, all diagnostic. None changes GPU behaviour when the new parameter
is left at its default 0.

### 1. BOS dump prints `bo->flags`, not `submit->bos[b].flags`

`a6xx_gpu.c` printed the wrong field all along. This tree predates
`struct drm_msm_gem_bo` (which does not exist anywhere in it) and uses the legacy
`msm_submit_rel` interface, so `submit->bos[b].flags` carries `BO_VALID |
BO_LOCKED | BO_PINNED | MSM_SUBMIT_BO_WRITE` — internal submit bookkeeping.
Observed `0x6003` decodes as exactly that, and is **not** a cache mode.

Consequence: no cache-mode correlation was ever actually under test, including
the image-249 `bo_mmap_wc` A/B. Now the line reads:

```
BOS[seq=N] bo[i] iova=... sflags=0x... boflags=0x... sz=... name='...' bo=...
```

`boflags` is the real cachiness: `MSM_BO_CACHE_MASK` = 0xf0000, with
`MSM_BO_CACHED` 0x10000, `MSM_BO_WC` 0x20000, `MSM_BO_UNCACHED` 0x40000.

### 2. `MSM_INFO_SET_NAME` actually stores the name

Was "accept and ignore". Added `char name[64]` to `struct msm_gem_object` and
copied from userspace (bounds-checked, `copy_from_user`). freedreno labels every
BO at creation — `"fs:…"`, `"vs:…"`, `"pvtmem_per_fiber"`, `"cmdstream"`,
`"gmem:…"` — so the dump becomes self-labelling and buffer identity stops being
inferred from size.

`len` counts bytes excluding the NUL, per upstream `fd_bo_set_name()`.

### 3. `bo_force_cached` module parameter (default OFF)

Set to 1 to rewrite `MSM_BO_WC`/`MSM_BO_UNCACHED` to `MSM_BO_CACHED` at
allocation, in one place, so every consumer of `bo->flags` agrees: the userspace
VMA prot in `msm_gem_mmap_obj()`, the vmap prot in `msm_gem_get_vaddr()`, and the
`sync_for_device()`/`sync_for_cpu()` decisions in `get_pages()`/`put_pages()`.

Doing it at allocation rather than per-consumer is deliberate — the earlier
coherency measurements went wrong precisely because the two sides disagreed.
Included so that, **if** the correlation confirms, the next boot needs no
rebuild:

```sh
echo 1 > /sys/module/msm_drm/parameters/bo_force_cached
```

## Also corrected in-tree

The `bo_mmap_wc` comment claimed "no effect has been measured (image 251)". That
measurement came from the pattern test, since shown to be a **null test** — the
buffers it wrote were never submitted, so the kernel never had a chance to
observe them. The comment now carries an explicit retraction. The parameter
stays, default 0, but "no effect has been measured" should be read as "never
properly measured".

## What this image decides

The one structural asymmetry still standing:

| BO | created by | alloc flags | content |
|---|---|---|---|
| bo[3], bo[5], bo[11] | kernel a6xx ring | `FD_BO_GPUREADONLY \| FD_BO_CACHED_COHERENT` | data |
| bo[1], bo[2] shader ISA | Mesa | `FD_BO_NOMAP` → `MSM_BO_WC` | **empty** |

freedreno marks exactly three things `FD_BO_NOMAP` (shader ISA, pvtmem, tess
factors) and `msm_bo.c` maps "no `CACHED_COHERENT`" to `MSM_BO_WC`. Every
GPU-only buffer is writecombine; every CPU-visible buffer is cached.

This image **measures that correlation for the first time**:

- ISA BOs show `boflags=0x20000`, command streams `0x10000` → lead confirmed,
  set `bo_force_cached=1` next boot and re-measure.
- Both show the same cache mode → lead dead, look elsewhere.
- `name='fs:...'` / `name='vs:...'` confirms identity without size inference; if
  the names are empty, Mesa is not calling `MSM_INFO_SET_NAME` and the UAPI
  argument passing needs a second look.

Expected to remain all-black on readback — nothing here changes GPU behaviour.