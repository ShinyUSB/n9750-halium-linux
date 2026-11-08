# Measurement validity — we do NOT actually know whether the GPU renders

Date: current session, image 234 (`kernel #41`, boot `ca4a5631`)

## The claim under review

"Every render target reads all zeros, therefore the GPU is not writing render
targets." This is asserted in `IMAGE-234-RESULTS.md`, `IMAGE-221-CLOCKS.md`
("DEFINITIVELY unwritten"), `MATRIX-RESULT.md` and `BO-CONTENT-TEST.md`.

**That inference does not follow.** Both instruments used to support it are
unsound, and a live experiment confirms the readback is not trustworthy.

## Instrument 1 — the kernel BOS[] dump is submit-time

The dump lives inside `a6xx_submit()`:

- `adreno/a6xx_gpu.c:421` — `static void a6xx_submit(...)`
- `adreno/a6xx_gpu.c:456` — `if (ibdump) { ... }`, the BOS walk

`a6xx_submit()` writes the command stream into the ring. It runs **before the
GPU executes any of it**. A render target sampled there is zero *by
construction*, so it cannot distinguish "GPU did not write" from "GPU has not
written yet".

Live confirmation of the sampling point (image 234, `ibdump=1` via
`/sys/module/msm_drm/parameters/ibdump`, mode 0644 so no rebuild needed):

```
BOS[seq=18] nr_bos=6 nr_cmds=1
BOS[seq=18] bo[1] iova=0x700025000 ndwords=5120     <- render target
bo[1]: nonzero=0 / dumped=5120
```

Only seq 17 and seq 18 appeared, and bo[1] (the render target) appeared
exactly **once** — a single pre-execution sample. `IMAGE-221-CLOCKS.md` treated
this as post-execution proof; it is not.

## Instrument 2 — glReadPixels is a GPU readback

freedreno implements `glReadPixels` as a GPU blit from a compressed render
target to linear memory. If the render-backend path is not executing, that blit
cannot execute either, and the result is zero **even when rendering is
perfectly correct**.

## Live experiment: sweep renderbuffer formats

`/root/fmtsweep.py` on device. Renders magenta (255,0,255) and reads back via
`glReadPixels(GL_RGBA, GL_UNSIGNED_BYTE)` into a correctly-sized
`W*H*4` buffer, one format per process.

| format | bytes/pixel | first 8 bytes | nonzero |
|---|---|---|---|
| RGBA8 (UBWC-compressible) | 4 | `[0,0,0,0,0,0,0,0]` | 0 / 16384 |
| SRGB8_ALPHA8 | 4 | `[0,0,0,0,0,0,0,0]` | 0 / 16384 |
| RGB8 | 3 | `[0,0,0,255,0,0,0,255]` | 4096 / 16384 |
| R8 | 1 | `[0,0,0,255,0,0,0,255]` | 4096 / 16384 |
| RGBA16F | 8 | FBO incomplete, no reading | — |

**No format returns magenta.**

The 1- and 3-byte formats show a repeating `[0,0,0,255]` — alpha only, RGB
zero, at exactly 25% of bytes. The 4-byte formats show nothing at all. The
readback is therefore **strongly format-dependent** and never reproduces the
colour that was cleared.

An earlier revision of this experiment appeared to show RGB8 and R8 returning
real data. That was a bug in the test: `glReadPixels(GL_RGBA, GL_UNSIGNED_BYTE)`
writes 4 bytes per pixel regardless of internal format, but the buffer was
allocated `W*H*bpp`, overflowing by 4 KB (RGB8) and 12 KB (R8). The "data" was
heap corruption, and it also caused a `double free or corruption`. Fixed by
always allocating `W*H*4`. Recorded here because the false result was briefly
treated as a breakthrough.

## Conclusion

**Rendering is not confirmed broken, and it is not confirmed working.** The
evidence supports neither. What is established:

- Address translation is real (see `RETRACTION-DMA-IOPMU.md`).
- Control-plane GPU writes land (`CP_EVENT_WRITE` timestamps, RPTR echo, the
  234 fence echo).
- Both available readback paths are unreliable: one samples before execution,
  the other depends on the very subsystem under test.

Any further "the render target is unwritten" claim is unsupported until a
measurement exists that does not depend on the render-backend path.

## RESOLVED — the GPU genuinely does not write (image 235)

The bypass measurement now exists and works: `/root/dmabuf.py` on device.

```
=== gbm_surface + dmabuf CPU readback ===
gbm device OK
gbm surface 64x64 OK
EGL 1.4
GL context on window surface OK
GL_RENDERER : FD640
cleared green, glGetError=0x0
swap done
bo 64x64 stride=256 fmt=0x34325258
dmabuf fd=7 -> reading from CPU memory

--- RESULT (CPU memory) ---
pixel[0,0]     = 0x00000000  (r=0 g=0 b=0 a=0)
nonzero pixels = 0 / 4096
VERDICT: GPU DID NOT WRITE THE SURFACE (all-zero buffer).
```

This satisfies both requirements:

1. **After execution** — `glFinish`, `eglSwapBuffers`, then
   `gbm_surface_lock_front_buffer`. The GPU has run.
2. **CPU memory** — the dmabuf fd is `mmap`ed and inspected directly. No GPU
   readback, so it cannot be invalidated by the RB path being broken.

**Conclusion: the GPU really does not write render targets.** The corpus's
conclusion was correct; it was previously resting on unsound evidence and is
now established on a trustworthy instrument.

Residual caveat, stated rather than hidden: if Mesa were rendering into an
internal staging buffer and never copying to the swapchain, this test would
also read zero. That is still "the GPU did not write this buffer", so the
finding stands either way — but it does not by itself distinguish "RB never
writes" from "a final blit is missing".

### How the test was built (for reproduction)

`EGL_EXT_image_dma_buf_import` is **absent** on this stack (`probe.py`), which
rules out the `gbm_bo` + `EGLImage` route. `EGL_KHR_image_base` and
`GL_OES_EGL_image` are present but useless without an import mechanism.

What works is `gbm_surface` + `EGL_PLATFORM_GBM_KHR`, with one Mesa quirk that
cost two attempts and a segfault:

- Passing the **`gbm_surface`** to `eglGetPlatformDisplayEXT` **segfaults**.
- Passing the **`gbm_device`** works, and the surface is then handed to
  `eglCreateWindowSurface` as the native window. This is what
  `pixelreadback.py:151` does for its FBO-only path.

The third argument must be `0`, not `None`.

## What a valid measurement needs

Read the render target from **CPU memory**, bypassing both instruments:

    render -> lock the buffer -> export dmabuf -> mmap -> inspect

No GPU blit, no submit-time sampling. Two routes:

1. `gbm_surface` + EGL window surface, then
   `gbm_surface_lock_front_buffer()` + `gbm_bo_get_fd()` + `mmap()`.
2. `gbm_bo_create()` + `EGL_EXT_image_dma_buf_import` /
   `GL_OES_EGL_image` (`glEGLImageTargetRenderbufferStorageEXT`), render into
   an FBO backed by that image, then `mmap` the `gbm_bo` directly.

Route 1 was attempted. It crashed in `eglGetPlatformDisplayEXT`; the working
EGL bootstrap on this device passes the **`gbm_device`** rather than a
`gbm_surface` and uses no surfaces at all (see `pixelreadback.py:151`:
`dpy = plat(0x31D7, dev, 0)` then `eglMakeCurrent(dpy, 0, 0, ctx)`). That rules
out route 1 as written and leaves route 2.

## Recommended next image (235)

**Measurement, not a fix.** Do not combine it with any speculative change.

- Implement route 2 userspace-side first. It needs no kernel work at all.
- If that proves rendering works, every "RB/SP never write" conclusion in the
  corpus is void and the investigation must restart from a working baseline.
- If it proves rendering fails, the RB/SP conclusion stands and the real
  defect search begins.