# IMAGE 220 + `FD_MESA_DEBUG=gmem` — the colour target points at UNMAPPED memory

Date: 2026-10-11
Image: `artifacts/flash/220-halium-boot-a6xx-bos.img` (no reflash; env-var A/B)
Captures: `artifacts/gpu-bringup/captures/220-run.txt`,
`artifacts/gpu-bringup/captures/220-gmem.txt`

## The A/B and why it was worth running

Predicted beforehand: "If GMEM renders and sysmem does not, nested IB fetch is
confirmed as the fault."

**The result refutes that.** Forcing the GMEM path still gives
`first pixel RGBA = (0,0,0,0)`. But the dump changed shape in a way that is far
more informative than the pixel value.

`FD_MESA_DEBUG=gmem` is the correct lever, verified in source
(`freedreno_gmem.c:704`):

```c
if (fd_autotune_use_bypass(&ctx->autotune, batch) && !FD_DBG(GMEM)) {
   sysmem = true;
}
```

## What changed

| | sysmem (default) | `FD_MESA_DEBUG=gmem` |
|---|---|---|
| clear mechanism | `CP_BLIT` via `RB_2D_DST_INFO` | `RB_MRT_BUF_INFO` colour target |
| colour target programmed | no | **YES** |
| pixel | (0,0,0,0) | (0,0,0,0) |

`RB_MRT_BUF_INFO` now appears at seq=4, 5 and 6 — the register that was
conspicuously absent from every earlier decode. So the GMEM path genuinely
programs a colour target, and it still produces a black pixel.

## THE FINDING: the colour target is not mapped

Decoded `RB_MRT` array (0x8822, 6 registers), seq=5 `bo[5]`:

```
[229] PKT4 0x8822 cnt=6          <- RB_MRT_BUF_INFO[0]
[230]   0x00000330               <- colour format (RGBA8)
[231]   0x00000004               <- pitch: 4 bytes/pixel  (correct)
[232]   0x00000100
[233]   0x00026000               <- *** BASE ADDRESS ***
[234]   0x00000000
[235]   0x00000000
[242] PKT4 GRAS_LRZ_MRT_BUF_INFO_0 = 0x00000030   <- colour format, now NON-ZERO
```

The base address is **`0x26000`**. Now the BO list for that same submit:

```
seq=4: 0x21000 0x2d000 0x17000 0x2a000
seq=5: 0x25000 0x21000 0x2d000 0x22000 0x17000 0x2b000
seq=6: 0x22000 0x25000 0x36000 0x2c000 0x17000 0x2a000
```

**`0x26000` does not appear in any of them.** The render target the GPU is
told to write is not among the buffers mapped for this submit.

This is the first hard, non-artefactual defect in the whole investigation, and
it is *not* one of the things previously suspected. It is invisible to the
sysmem path because that path addresses the destination through
`RB_2D_DST_INFO` instead — which is exactly why image 220's sysmem capture
looked perfectly correct and still did nothing.

## What this means

Two independent colour-target mechanisms, two independent ways of failing to
have the render target mapped:

- **sysmem:** blit to `RB_2D_DST_INFO` dest `0x36000` — which *was* a real BO
  (`bo[2] iova=0x36000` in seq=3). So sysmem's destination looked valid, and
  yet the blit did not happen.
- **gmem:** `RB_MRT` base `0x26000` — which is **not** a BO at all.

The common factor is therefore probably *not* the colour address. It is more
likely that BOs are being allocated with GPU addresses that are not actually
mapped into the GPU address space, or that the mapping is stale by the time the
GPU runs the command stream.

## Next discriminator (the one I would run next)

Dump `bos[].obj`'s real mapped GPU VA (`msm_gem_iova()`) and compare it against
`bo->base.size` and the `0x26000`/`0x36000` values the stream actually uses.
If the stream's addresses are offsets into a BO rather than the BO's own VA,
that is a reloc/patching bug and it would explain *both* paths failing at once.

Second, cheap and orthogonal: read the render target back with
`glReadPixels` **twice** — once now, once after a `glFinish()` and an `mt/msync`
of the staging buffer — to separate "GPU never wrote" from "write happened but
readback is broken". The `handle_rgba_blit` UBWC→linear path remains a live
suspect for the readback half.

## Note on the earlier hypothesis

The nested-IB-fetch theory from IMAGE-220-CLEAR-IS-CORRECT.md is **weakened,
not confirmed**. The GMEM path also nests its commands, and it also fails, so
nesting alone does not explain the failure. I should not have predicted that
the A/B would discriminate cleanly — both paths share the nesting property, so
the test could not have separated them. The colour-target-mapping defect is a
better lead and it is consistent with everything so far.