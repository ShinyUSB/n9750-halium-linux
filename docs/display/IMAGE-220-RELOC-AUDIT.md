# Image 220 follow-up — local reloc-path audit

## What was checked locally

### BO flags are healthy
Every BO in every submit showed `flags=0x6003` or `0x6007`:

```
0x6003 = BO_LOCKED(0x4000) | BO_PINNED(0x2000) | READ(0x1) | WRITE(0x2)
0x6007 = same + 0x4
```

`BO_VALID` (**0x8000**) is **absent from every BO in every submit**. Per
`msm_gem_submit.c:359-360`:

```c
ret = submit_bo(submit, submit_reloc.reloc_idx, NULL, &iova, &valid);
if (valid)
        continue;                 /* <-- skips patching entirely */
```

So with `BO_VALID` clear, **every reloc was actually applied**. This rules out
the "stale address trusted from the commandstream" theory — it is not happening
here.

`submit_pin_objects()` (msm_gem_submit.c:252-278) sets `BO_VALID` only when the
BO's iova is unchanged since the last submit; on change it clears `BO_VALID`
and marks `submit->valid = false`. That logic looks correct and is behaving
correctly.

### No reloc errors
No `DRM_ERROR` output (`reloc`, `invalid buffer index`, `non-aligned`) appears
in any capture. The patching path is running clean.

## The outstanding anomaly, restated precisely

`RB_MRT_BASE` (reg **0x8825**) = **`0x26000`** in every GMEM submit (seq 4, 5,
6 — verified by three independent decodes of the raw dwords).

The complete set of GPU VAs appearing anywhere in `bos[]` across the whole run:

```
0x17000 0x21000 0x22000 0x25000 0x2a000 0x2b000 0x2c000 0x2d000 0x36000
```

**`0x26000` appears nowhere.** Not in any `bos[]` entry, not in any command
stream, anywhere in a 12k-line capture.

Mesa emits it as a reloc (`A6XX_RB_MRT_BASE(i, .bo = rsc->bo, .bo_offset = offset)`,
`fd6_gmem.c:133`) via `fd_ringbuffer_attach_bo(ring, regs[i].bo)`
(`fd6_pack.h:68`), so the BO is supposed to become a submit dependency and gain
a `bos[]` entry.

Also worth noting: `0x26000` is exactly one page (`0x1000`) above `0x25000`,
which is a BO in the same submit. Suggestive of an off-by-a-page or a
stale/from-different-submit address, but **not proven** — `0x26000` could
equally be a legitimately different allocation from a submit we did not capture.

## What could not be verified locally

`fd_ringbuffer_attach_bo()` is referenced in `a6xx/fd6_pack.h` but its
implementation is **not present in this Mesa tree** (it lives in
`freedreno_ringbuffer.c`, which is absent from the checkout at
`/home/usb/.opencode-tmp/mesa2236`). So I cannot locally confirm whether an
attached BO is always registered as a submit dependency, or whether some path
attaches without creating a `bos[]` entry. That is the specific gap.

## Status

Two candidate explanations remain, and they are not yet distinguished:

1. The `RB_MRT_BASE` reloc is patched with a **stale or wrong VA** — i.e. the
   render target is genuinely programmed to unmapped memory. Explains the GMEM
   path failing.
2. The BO is correctly mapped and the address is fine, and the failure is
   elsewhere (e.g. 2D blit execution).

Note the sysmem path's `RB_2D_DST_INFO` dest (`0x36000`) **was** present in
`bos[]`, so that path's address was valid — yet it also failed. That asymmetry
is the key puzzle: one path has a bad address, the other has a good address,
and neither works. It keeps pointing at something common to both (blit
execution) rather than at addressing alone.

## Next

Awaiting the Mesa/kernel research on blit execution requirements
(`RB_CCU_CNTL.color_offset`, `SP_MODE_CONTROL`, UBWC init) before choosing
between (1) and (2). The most decisive kernel-side experiment would be to dump
each BO's freshly-computed `msm_gem_iova()` at submit time and compare against
both `bos[].iova` and the `0x26000` found in the stream.