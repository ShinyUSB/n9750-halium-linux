# The missing batch: complete source analysis and fix shape

Date: 2026-11-11. Mesa 22.3.6 at `/tmp/opencode/mesa2236`.
Upstream verification pending (consultant round in flight).

## Where a batch is supposed to come from

The only creator in the a6xx driver:

```
freedreno_context.c:291  fd_context_batch()
  -> fd_batch_from_fb(ctx, &ctx->framebuffer)
  -> fd_batch_reference(&ctx->batch, batch)
                         ...
  freedreno_batch_cache.c:426  batch = fd_batch_create(ctx, nondraw)
                                                 -> fd_batch_from_fb() -> key on framebuffer
```

`fd_context_batch()` copies `ctx->framebuffer` into the new batch. So the
creation order is: the *context* framebuffer is set first, then the first thing
that needs to emit calls `fd_context_batch()` and gets a batch that inherits the
framebuffer state.

## Who calls it (exactly one caller in a6xx)

```
a6xx/fd6_emit.c:1416  fd6_framebuffer_barrier()
     struct fd_batch *batch = fd_context_batch_locked(ctx);
```

That is an emit-time helper, not something a clear or a draw reaches.

## The functions that use `ctx->batch` without creating it

`a6xx/fd6_draw.c` alone:

| line | use | who calls it |
|---|---|---|
| 193 | `fd6_vsc_update_sizes(ctx->batch, ...)` | draw_vbo |
| 241 | `ctx->batch->draw` | draw path |
| 278 | `ctx->batch->tessellation = true` | draw path |
| 325-332 | `fd_reset_wfi(ctx->batch)`, `ctx->batch` | draw path |
| **471** | **`&ctx->batch->framebuffer`** | **fd6_clear** |
| 483 | `ctx->batch->num_draws` | fd6_clear |
| 487-493 | `ctx->batch->clear_color/depth/stencil/fast_cleared` | fd6_clear |
| 500 | `fd6_clear_lrz(ctx->batch, ...)` | fd6_clear |

Plus the whole `fd6_emit.c` surface (`ctx->batch->framebuffer` at 251, 598, 631,
757; `ctx->batch->submit` at 539, 567, 714, 737, 762; `ctx->batch->fb_read_patches`
at 255) — all of which are called *during* emit, after a batch exists.

## So what normally creates the batch before clear/draw?

On a typical path (e.g. weston, glmark), the sequence is:

```
eglMakeCurrent / eglSwapBuffers
  -> ... some state that reaches fd_context_batch() or fd6_framebuffer_barrier()
     -> batch exists
  -> fd6_clear() / fd6_draw_vbo() use ctx->batch safely
```

What is different about *our* path is that it is a fresh context, a fresh FBO,
and a clear as the first operation — and `fd_set_framebuffer_state()` explicitly
does `fd_batch_reference(&ctx->batch, NULL)` when the framebuffer changes
(`freedreno_state.c:300`). So on this specific first-operation path there is no
batch, and `fd6_clear` is the first thing that touches `ctx->batch`.

## The fix

One line at the top of `fd6_clear`, matching the one existing template:

```c
 static bool
 fd6_clear(struct fd_context *ctx, unsigned buffers,
           const union pipe_color_union *color, double depth,
           unsigned stencil) assert_dt
 {
+   struct fd_batch *batch = fd_context_batch_locked(ctx);
    struct pipe_framebuffer_state *pfb = &ctx->batch->framebuffer;
```

…with the rest of the function switched to `batch` where it reads more clearly
(`num_draws`, `clear_color`, `fast_cleared`, `fd6_clear_lrz(batch, ...)`).

`fd_context_batch_locked()` (not the unlocked variant) is correct because
`fd6_clear` is `assert_dt` — driver thread, emit lock needed.

Open question for the upstream-verification round: whether newer Mesa has this
fix already, and whether the draw paths (`fd6_draw_vbo` etc) have the same latent
bug on a first-operation path. If the draw fails identically after the clear is
fixed, the same one-liner may be needed at the top of the draw path — but that is
a bridge to cross after pixels, not before.

## Why this is not "we need a newer Mesa"

A Mesa *upgrade* would also fix this (if upstream has), but it is a far larger
change than needed. The phone runs Mesa 22.3.6, the source matches the binary
exactly, and the fix is one line in one function. Build that one object, install
`msm_dri.so`, re-run `glstage 6`, and expect pixels.
