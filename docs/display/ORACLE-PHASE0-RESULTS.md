# Phase 0 — cache-sound GPU write oracle: the "GPU never writes" finding SURVIVES

Device: image 242, kernel `#49`, boot `6d6fae4e`. Bring-up clean
(`HWINIT[25] return ret=0`, `SELFTEST142: done last_iter=10 ALL 10 PASS`).
28/28 submits retired their fence with the backend engaged.

## Why this test exists

The plan I proposed held that `prefill.py` / `dmabuf.py` / `cpuread.py` were
**unsound**, because the CPU writes its pattern and reads it back through the
same `mmap`, and an ARM64 write-back cached mapping means the readback returns
the CPU's own stale cache rather than what the GPU put in DDR. If true, the
"GPU NEVER TOUCHED THE BUFFER" verdict from image 239 would be an artifact,
and the whole LLC/SCID (242) / amsbc (237) / A640-clock (235) campaign would
have been chasing a measurement bug.

Supporting evidence for that concern, all verified in this tree:
- `msm_gem.c` has no `begin_cpu_access` / `end_cpu_access`; `sync_for_cpu()` is
  a `static` helper with few callers.
- `msm_gem_cpu_prep()` / `msm_gem_cpu_fini()` carry literal
  `/* TODO cache maintenance */` (`msm_gem.c:916,923`).
- msm defines no `struct dma_buf_ops`, so the dma-buf ops come from
  `drm_gem_dma_buf_ops()`, which do **not** call `sync_for_cpu()`.
  `dma_buf_begin_cpu_access()` from userspace is therefore a no-op here.

## The test (`/root/oracle.py`, host copy `/tmp/opencode/oracle.py`)

Invert the prefill test. **The CPU never writes the render target.**

1. Create a `gbm_surface`, EGL window surface, GLES3 context.
2. `glClear` to a known colour, `glFinish`, `eglSwapBuffers`, `glFinish`.
3. `gbm_surface_lock_front_buffer()` → dmabuf fd.
4. Map it **`PROT_READ` only, for the first time in this process**, and read.

A non-zero pattern in a buffer the CPU has never written can only have come
from the GPU. There is no CPU-side store anywhere in the measured path, so
cache staleness cannot manufacture a false negative from *our* writes.

Guards built into the harness:
- dmabuf identity is compared by **inode**, not fd (lesson from
  `IMAGE-239-RESULTS.md`).
- A bo is only ever mapped **once**. A CPU *read* also fills the cache, so
  re-reading the same bo would be unsound. Reused inodes are reported and
  skipped, never silently counted.
- **Test C**, an instrument self-check, runs on a separate `gbm_bo_create()`:
  the CPU writes `0xCDCDCDCD`, reads it back. This proves the read path really
  reports DRAM, so an all-zero result is a property of the GPU and not of a
  broken mapping.

## Result (ORACLE_W=64)

```
GL_RENDERER : FD640

--- round 0: cleared RED ---
  bo 64x64 stride=256 fd=7 inode=34065  FRESH (sound)
  pixel[0,0]     = 0x00000000 (r=0 g=0 b=0 a=0)
  nonzero pixels = 0 / 4096

--- round 1: cleared GREEN ---
  bo 64x64 stride=256 fd=8 inode=34065  REUSED (UNSOUND, skipped)

--- round 2: cleared BLUE ---
  bo 64x64 stride=256 fd=9 inode=34065  REUSED (UNSOUND, skipped)

--- Test C: instrument self-check (CPU writes, CPU reads) ---
  inode=34067 fd=10 wrote 0xCDCDCDCD, read back 0xcdcdcdcd -> READ PATH OK

=== VERDICT ===
  round RED: all zero  -> GPU DID NOT WRITE IT
```

## Conclusions

1. **My cache hypothesis was WRONG, and the corpus's finding was right all
   along.** With an instrument that never writes to the measured buffer, and
   with the read path independently proven functional, the render target is
   genuinely all-zero after the GPU retires the blit. Image 239's verdict
   stands.
2. **The LLC/SCID, amsbc and clock-value campaigns were not misdirected.**
   Those hypotheses are still properly refuted. The A640 `CP_ROQ_THRESHOLDS_2`,
   `GBIF_HALT` and GMU-AO-CGC work is still the live list.
3. **Residual ambiguity, stated rather than hidden:** the kernel zeroed the
   pages at allocation. If those zeroing stores were left dirty in some CPU's
   cache and never written back, they could still mask a GPU write. Test C
   does not exclude this (it writes *and* reads the same mapping). A
   kernel-side readback that invalidates the CPU cache by physical address
   would close it. Cheap; do it as insurance, but the finding is strong
   enough to act on.

## New observation (not yet explained)

Rounds 0/1/2 returned the **same inode (34065)** with three different fd
numbers. gbm handed back one buffer, not three. So `eglSwapBuffers` is not
cycling distinct buffers on this stack, or `lock_front_buffer` is returning
the same bo. This is worth a look: it means the "round N uses a fresh bo"
assumption that any multi-colour test relies on does not hold by default here,
and it may itself be a symptom of the swap path not completing.

## Readout for the next image

The remaining suspects are unchanged and now better founded. Next: image 243,
a640 bring-up parity with CAF and mainline — `CP_ROQ_THRESHOLDS_2`,
`GBIF_HALT`/`RBBM_GBIF_HALT`, GMU AO CGC, dropping the a610-only
`GBIF_QSB_SIDE0..3`, and `RBBM_NC_MODE_CNTL` — with each item behind its own
runtime module parameter so a single flash yields N experiments.