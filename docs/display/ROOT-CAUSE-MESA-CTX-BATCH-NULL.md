# 🎯 ROOT CAUSE (definitive, in Mesa source): `ctx->batch` is NULL when `fd6_clear` runs

Date: 2026-11-11. Mesa 22.3.6 at `/tmp/opencode/mesa2236` (re-clone; `/tmp` does
not survive reboot).

## The defect, exactly

`src/gallium/drivers/freedreno/a6xx/fd6_draw.c:467`

```c
static bool
fd6_clear(struct fd_context *ctx, unsigned buffers,
          const union pipe_color_union *color, double depth,
          unsigned stencil) assert_dt
{
   struct pipe_framebuffer_state *pfb = &ctx->batch->framebuffer;   /* <-- ctx->batch may be NULL */
```

`fd6_clear` dereferences `ctx->batch` **without ever obtaining one**. In the whole
a6xx driver there is exactly one caller of `fd_context_batch()` —
`fd6_framebuffer_barrier()` (`fd6_emit.c:1416`) — and a clear never reaches it.

## Why `ctx->batch` is NULL at clear time

`src/gallium/drivers/freedreno/freedreno_state.c:268`, `fd_set_framebuffer_state()`,
deliberately **drops** the batch when the framebuffer changes:

```c
   if (ctx->screen->reorder) {
      struct fd_batch *old_batch = NULL;
      fd_batch_reference(&old_batch, ctx->batch);
      if (likely(old_batch))
         fd_batch_finish_queries(old_batch);
      fd_batch_reference(&ctx->batch, NULL);      /* <-- batch released here */
      fd_context_all_dirty(ctx);
      ...
   }
```

So the sequence for a fresh FBO + clear is:

```
glBindFramebuffer / glClear
  -> fd6_set_framebuffer_state()      fd6_context.c:128
     -> fd_set_framebuffer_state()     freedreno_state.c:268
        -> fd_batch_reference(&ctx->batch, NULL)      batch is now NULL
  -> ctx->clear(ctx, ...)              fd6_draw.c:512 -> fd6_clear()
     -> &ctx->batch->framebuffer       ctx->batch == NULL
```

The batch is only ever (re)created by `fd_context_batch()`
(`freedreno_context.c:291`), which allocates from `ctx->framebuffer` — i.e. it is
created lazily by the *first thing that needs to emit*. A clear that only records
`clear_color[]`/`clear_depth` into the batch assumes a batch already exists, and
nothing on this path creates one.

## This exactly matches the observed behaviour

| observation | explanation |
|---|---|
| `FD_MESA_DEBUG=msgs` shows `fd_screen_create`, format probing, `fd_resource_allocate_and_resolve` — but **no `fd_batch_create`** | the batch layer is never entered |
| `dmesg \| grep -c GEM_SUBMIT` = **0** | no batch → no `fd_submit` → no ioctl |
| `RB_RPTR == RB_WPTR == 0x63` before *and* after `glClear` | no commands were ever emitted |
| `A640` IRQ count 0 in all 8 CPU columns | nothing completes, so nothing raises an interrupt |
| `glGetError` = `0x501` (`GL_INVALID_VALUE`) on the clear | the clear is being rejected, consistent with a broken/uninitialised batch path |

And it explains the historical notes exactly. `DEFERTEST-RESULTS.md` recorded
`fd_batch_create` firing **once** then stopping — that first batch came from some
other emit path (blit/shader), not from the clear. The documented open question,
"why does the flush stop after `fd_batch_resource_write`, before
`fd_submit_sp_flush`", is the same missing-batch problem seen from the other side.

## The fix

Acquire a batch at the top of `fd6_clear`, the same way every other emit entry
point does. The minimal, upstream-idiomatic change:

```c
 static bool
 fd6_clear(struct fd_context *ctx, unsigned buffers,
           const union pipe_color_union *color, double depth,
           unsigned stencil) assert_dt
 {
+   struct fd_batch *batch = fd_context_batch_locked(ctx);
    struct pipe_framebuffer_state *pfb = &ctx->batch->framebuffer;
```

…with the remaining `ctx->batch` uses in that function (`fd6_clear_lrz(ctx->batch,
…)`) switched to `batch` for clarity. `fd_context_batch_locked()` is the correct
variant because `fd6_clear` is already `assert_dt` (driver thread).

This is a **one-line userspace change** — no kernel rebuild, no reflash.

## Before building it

1. Confirm the diagnosis on-device rather than on paper: `ctx->batch` should read
   NULL. The cheapest proof is that no `fd_batch_create` line appears in
   `FD_MESA_DEBUG=msgs` output for a clear-only workload — already observed.
2. Check whether upstream Mesa has since fixed this (search did not surface a
   matching commit, but 22.3.6 is from Oct 2022 and later branches may differ).
   If newer Mesa has the fix, the better move may be to backport the whole fix
   rather than write our own.
3. Build Mesa on the phone (it is aarch64, so this is native, not cross) and
   install `msm_dri.so`. `MESA-SOURCE-ANALYSIS.md` records that Mesa 22.3.6 is
   the exact version on the phone, so the source matches the binary.

## Expected outcome

A batch is created on clear → `fd_submit_flush()` runs → `GEM_SUBMIT` ioctl →
`RB_RPTR` advances past `0x63` → the A640 IRQ fires for the first time →
`glReadPixels` returns real pixels.

That is the last step between this project and a rendered image.

## Do not

- Do not revert image 202. The coherency fix is proven 3-for-3 and is correct.
- Do not re-derive the IRQ theory: the IRQ is a *symptom* of nothing being
  submitted. `ROOT-CAUSE-GPU-IRQ-NEVER-FIRES.md` is superseded by this file.
- Do not re-derive the `pm_runtime_work` stall (`DECISIVE-CPU-LOAD-TEST.md`) or the
  apps-watchdog theory (`watchdog_v2.enable=0`, never ran).
- Do not resume the retracted `batches[32]`/`ffs()` overflow theory.
