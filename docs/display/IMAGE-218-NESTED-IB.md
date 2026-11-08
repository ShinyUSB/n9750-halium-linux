# IMAGE 218 — the real clear submit, and the blind spot one level deeper

Date: 2026-10-11
Image: `artifacts/flash/218-halium-boot-a6xx-ibdump-ctrl.img`
Captures: `artifacts/gpu-bringup/captures/218-run.txt`, `218-preboot.txt`

## What image 218 fixed

Image 217 gated all diagnostics on `submit->seqno <= 3`, so after three
submits the dump went silent forever. Image 218 replaced that with a runtime
module parameter `ibdump` (default **N**, zero cost).

**Param path: `/sys/module/msm_drm/parameters/ibdump`** — it is a DRM-module
param, *not* under `/sys/module/a6xx/`. (I initially looked in the wrong
place and briefly concluded the build was broken; it was not.)

Verified working: set `1` → `Y` before the run, `0` after. The GPU was brought
up with `echo 1 > .../a6xx_gpu_load` and reached `BABEFACE ms~1`,
`hw_init ret=0`.

## Result: THREE submits, three different sizes

This immediately proves image 217's 319-dword stream was a stale artifact of
the very first run and had nothing to do with the clear:

| submit | dwords | iova |
|---|---|---|
| seq=1 | **346** | 0x2a000 |
| seq=2 | **332** | 0x2b000 |
| seq=3 | **223** | 0x2a000 |

Still `first pixel RGBA = (0,0,0,0)`.

## THE FINDING: the render commands are in nested indirect buffers

seq=1 (346 dwords) contains **three `CP_INDIRECT_BUFFER` (opcode 0x3f)
references**, not just its own commands:

| dword | iova | size |
|---|---|---|
| 199 | 0x2c000 | **77 dwords** |
| 304 | 0x2d000 | 18 dwords |
| 316 | 0x2d050 | 19 dwords |

None of these buffers appeared in the capture. Why: they are submitted as
`MSM_SUBMIT_CMD_IB_TARGET_BUF`, and per
`include/uapi/drm/msm_drm.h`:

> `IB_TARGET_BUF - this cmd buffer is an IB target. Reloc's are processed
> normally, but the kernel does not setup an IB to this buffer in the
> first-level ringbuffer`

So the kernel correctly does not emit a ring reference for them — and image
218's dump, which iterated `nr_cmds` and handled only `CMD_BUF`, skipped them
for exactly the same reason.

**This is the same class of blind spot image 218 fixed one level up.** The one
buffer we have been decoding is only ever the envelope. The clear
(`batch->tile_setup`) and the draw/prologue buffers are all IB targets.

This also fully explains image 217's "empty framebuffer" finding: `emit_mrt`'s
tail registers and `emit_zs`'s no-zsbuf writes live in the *outer* buffer,
while the actual colour target and clear are in the nested ones. We were
reading the wrapper and calling it the contents.

## Image 219

Adds an `IBTGT[...]` dump for `MSM_SUBMIT_CMD_IB_TARGET_BUF` — same mapping
approach, dumping the target buffer contents. That should finally show the real
clear (expected in the 77-dword buffer) and, critically, the
`RB_MRT_BUF_INFO(0)` colour target and `RB_BLIT_INFO`/`CLEAR_MASK` that have
been missing from every decode so far.

## Run log discipline used (worth keeping)

1. bank dmesg to host before any trigger
2. `echo 1 > .../a6xx_gpu_load`  — alone in its command, no sleep
3. `echo 1 > /sys/module/msm_drm/parameters/ibdump`  — before the GL run
4. glstage **once**
5. `echo 0 > .../ibdump`, then capture

Note the phone froze after the GPU trigger on this image too (boot_id
`f44397b6` → manual reboot → `3a6c64b8`), so captures are pulled immediately
after bring-up, before the GL run.