# Image 258 — results

```
artifacts/flash/258-halium-boot-a6xx-boflags.img
sha256 999fb8eb8828136bf265c13bcc5b4baa1eadf916e3192a8bfc588f5ff4360d91
kernel 4.14.190-ShinyKernel
```

## 0. The GPU does not come up on its own — and this nearly produced a false breakthrough

The first run after flashing reported:

```
PIXEL=[0,0,255,255]   (want clear 0,0,255,255 or shader colour)
```

The first non-black pixel in the entire project. **It was llvmpipe.**

```
MESA: error: get_param:232: get-param failed! -6 (No such device or address)
MESA: error: fd_pipe_new2:52: allocation failed
renderer=b'llvmpipe (LLVM 15.0.6, 128 bits)'
```

`priv->gpu` is NULL until the GPU is explicitly loaded, so
`msm_ioctl_get_param` returns -ENXIO (-6), `fd_pipe_new2` fails, and Mesa falls
back to software. Software renders a `glClear` perfectly. The correct bring-up
chain, from `CLEAN-SESSION-RECIPE.md`:

```sh
./u.sh --fix
echo on > /sys/devices/platform/soc/2c00000.qcom,kgsl-3d0/power/control
nohup /root/poller.sh >/dev/null 2>&1 &
echo 1 > /sys/devices/platform/soc/ae00000.qcom,mdss_mdp/a6xx_gpu_load
```

After that: `SELFTEST142: done last_iter=10 ALL 10 PASS`, `HWINIT[25] return
ret=0`, `renderer=b'FD640'`. `control=on status=unsupported` *before* GPU use is
expected, not a fault.

**`bigshader.py` had no `FD640` guard** and so sailed straight past the
fallback. Guard added to it (only 6 of ~100 harnesses had one). Any pixel from
an unguarded harness is worthless until the renderer string is checked.

## 1. Cache-mode hypothesis: REFUTED

The whole point of image 258. With the corrected dump field:

```
bo   boflags  size     nonzero/dwords
0    0x20000  4096         1/1024
1    0x20000  20480        0/5120     <- shader ISA
2    0x20000  4096         0/1024     <- shader ISA
3    0x20002  4096       72/1024
4    0x20000  4096         1/1024
5    0x20002  4096      149/1024
11   0x20002  4096      244/1024

distinct cachemodes across ALL BOs: ['0x20000']
```

**Every buffer is `MSM_BO_WC` (0x20000).** There is no cache-mode asymmetry
between the command streams that have data and the shader ISAs that are empty.
The proposal — "freedreno marks exactly three things `FD_BO_NOMAP`, those get
WC, and those are the empty ones" — is **dead**.

The only flag that separates them is `GPU_READONLY` (0x2): present on bo[3],
bo[5], bo[11] (all with data), absent on the shader ISAs. That is just Mesa's
own alloc-flags choice distinguishing GPU-read-only buffers from writeable ones,
not a memory-attribute difference.

This is what the corrected field bought: the hypothesis is refuted for the cost
of one build, instead of surviving as a plausible-sounding theory. It would
have been unfalsifiable before, because the dump was printing
`submit->bos[b].flags`.

`bo_force_cached` is therefore left unused. Mapping everything cached is no
longer a candidate explanation, since the working buffers are cached as little
as the broken ones.

## 2. BO names: empty, and that is correct

`name=''` on every BO. Not a kernel bug:

```c
static inline struct fd_bo *
fd_bo_new(struct fd_device *dev, uint32_t size, uint32_t flags, const char *fmt, ...)
{
   struct fd_bo *bo = _fd_bo_new(dev, size, flags);
#ifndef NDEBUG
   if (fmt) { ... _fd_bo_set_name(bo, fmt, ap); ... }
#endif
   return bo;
}
```

`fd_bo_set_name` / `fd_bo_new`'s naming path is inside `#ifndef NDEBUG`. The
Debian Mesa package is a release build, so **`MSM_INFO_SET_NAME` is never
issued**. `msm_bo_set_name()` even has its own guard:

```c
if (bo->dev->version < FD_VERSION_SOFTPIN) return;
```

— and the version handshake is fine: a correctly-built probe (`ioctl 0xC0406400`,
64-byte CAF `struct drm_version`) returns **major 1, minor 4, patchlevel 0**,
i.e. exactly `FD_VERSION_SOFTPIN`. So it is the `NDEBUG` guard, not the version.

The kernel implementation is correct and will work if Mesa is ever rebuilt with
`NDEBUG` undefined. With the stock release Mesa it is unreachable, so BO
identification must continue to rely on size/iova inference.

## 3. The ISA BOs are still empty

`bo[1]` (20480 B) 0/5120 nonzero, `bo[2]` (4096 B) 0/1024 nonzero, with the
shader cache deleted, `MESA_SHADER_CACHE_DISABLE` + `MESA_GLSL_CACHE_DISABLE`
set, cold compile, GPU genuinely on FD640, PM held. Unchanged.

Note bo[0] and bo[4] have exactly **1** nonzero dword — the subagent's point
again: a single reloc/header word is not content. Effectively all four
non-command-stream buffers are empty.

## 4. Upload path: mmap sizes

strace with the cache deleted, counting every mmap on the DRM fd:

```
20 x 4096
 2 x 32768
 0 x 20480
```

There is no 20480-byte mmap. But there are exactly **two** shader BOs, and
20480 aligns up to 32768 at 16 KiB (VPC) granularity, so the two `0x8000` mmaps
are most likely the two ISAs. Consistent with `fd_bo_upload` running:

```c
memcpy((uint8_t *)bo_map(bo) + off, src, len);   /* no NULL check */
```

A NULL would segfault; nothing crashes. So the memcpy executes into a valid
userspace VMA.

This is not yet proof — it rests on the 16 KiB alignment inference. It is
directly testable by allocating a UBO of an unmistakable size and confirming
that size appears among the submitted BOs before comparing content, which is
the flaw that invalidated `pattern.py`.

## Status

Unchanged from image 257: CP retires everything, no faults, no pixel. The
shader ISA never reaches DRAM as seen by the kernel, and the upload path
appears to execute correctly. The gap is now narrowed to "userspace writes a
mapping that the kernel does not read back" — but that must be re-tested with a
harness that *proves its buffer was submitted* before claiming it again.