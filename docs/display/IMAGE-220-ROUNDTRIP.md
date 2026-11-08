# IMAGE 220 follow-up — round trip: readback AND clear both fail, plus a texture-attach bug

Date: 2026-10-11
Image: 220 (no reflash)
Harness: `artifacts/tmp-rescue/harness/roundtrip.py` (new)

## Test design

Every result so far — `(0,0,0,0)` after `glClear` — is ambiguous. It is equally
consistent with:

  (a) the GPU never performed the clear, or
  (b) the clear worked and the **readback** is broken.

To separate them we need a known-good CPU→GPU→CPU round trip. If a CPU-uploaded
pattern reads back correctly, the readback path is proven working and any
remaining black pixel must be a real GPU-execution failure.

## Result: the round trip FAILS. Readback is broken.

```
=== RGBA8 texture ===
  gen/bind/param err  = 0x0
  glTexImage2D err    = 0x0  (ok)
  glIsTexture(tex=1)  = 1
  tex level0 64x64 err= 0x0
  ROUND TRIP  want ~(200,100,50,255)  got (0,0,0,0)   READBACK BROKEN
```

The CPU uploaded a known pattern via `glTexSubImage2D` with **no error**, the
texture is valid, 64x64, and reading it back yields all zeros.

**This is the key result: `glReadPixels` does not work at all on this setup,
independent of the clear.** Every "black pixel" measurement in this project —
including the very first ones — has therefore been measuring the *readback*,
not the render.

So the two remaining candidates collapse into:

1. the GPU does not execute the clear, and
2. the readback does not report what is in memory.

...and we can no longer distinguish them until readback is fixed. The
`handle_rgba_blit: 2d r8g8b8a8_unorm UBWC -> linear` path seen in the debug log
is now the **prime suspect**, because it is the readback path.

## NEW BUG FOUND: glFramebufferTexture2D rejects a valid texture

Unrelated to the above but important, and reproducible:

```
  glIsTexture(tex=1)  = 1          <- texture IS valid
  tex level0 64x64 err= 0x0        <- storage IS allocated
  rebind err          = 0x0
  fboattach err       = 0x501       <- GL_INVALID_VALUE
  status              = 0x8CD7      <- INCOMPLETE_MISSING_ATTACHMENT
```

Every argument is valid (`GL_FRAMEBUFFER`, `GL_COLOR_ATTACHMENT0`,
`GL_TEXTURE_2D`, a live texture name), the texture is bound and has storage,
and the call still fails with `GL_INVALID_VALUE` and the attachment does not
take.

`FD_MESA_DEBUG=msgs` shows the mechanism:

```
fd_resource_allocate_and_resolve: target=2d, r8g8b8a8_unorm, 64x64x1, usage=0, bind=a
fd_resource_allocate_and_resolve: target=2d, r8g8b8a8_unorm, 64x64x1, usage=4, bind=20000a
```

The first allocation has `bind=a` = `PIPE_BIND_SAMPLER_VIEW` only. After the
attach attempt a **second** allocation appears with `bind=2000a`, whose `0x2000`
is `PIPE_BIND_RENDER_TARGET`. So Mesa *does* allocate render-target storage —
but the framebuffer attachment still ends up missing.

This is a genuine state-tracker/driver bug on the render-target binding path,
and it is the same subsystem that would explain the earlier
`RB_MRT_BUF_INFO` base address `0x26000` not appearing in the submit's BO list:
the render-target binding is not correctly reaching the pipe state.

Two independent observations now point at the same place:
- `RB_MRT` base `0x26000` is not among the submit's BOs (IMAGE-220-GMEM-AB.md)
- `glFramebufferTexture2D` fails to attach a valid texture

Also note `GL_RGB565` fails even earlier — `glTexImage2D` returns `0x501` for a
format freedreno should support, suggesting the texture-format table itself has
issues on this device.

## Conclusion

The investigation has been measuring the wrong thing. `glReadPixels` returns
zeros even when the CPU itself just wrote known data through the GPU. Until
that is fixed, **no pixel-based conclusion from this harness is trustworthy**,
including "the clear did not execute".

The render-target binding path (`bind=2000a` allocation not reaching the
framebuffer, and `RB_MRT` base `0x26000` absent from the BO list) is now the
strongest lead, and it is a real defect rather than a measurement artefact.

## Next steps

1. Check whether `glReadPixels` works on a **linear** (non-UBWC) buffer, and on
   the default framebuffer — that would separate the UBWC conversion from
   readback generally.
2. Investigate why the render-target allocation (`bind=2000a`) is not attached —
   this now looks like the primary defect.
3. Only once readback returns known data can "does the clear execute" be
   answered at all.