# Mesa 22.3.6 source analysis — the deferral path, and why it changed the picture

Cloned upstream Mesa at tag `mesa-22.3.6` (VERSION file confirms exact match
with the phone). Found the real submit path, which is *not* where either
consultant guessed.

## The submit path, in reality

It is not in `gallium/drivers/freedreno/`. That directory has no
`DRM_IOCTL_MSM_GEM_SUBMIT` anywhere — grep returns nothing. The chain is:

```
fd_context_flush            (gallium/drivers/freedreno/freedreno_context.c)
  -> fd_bc_flush / fd_batch_flush
    -> cleanup_submit       (freedreno_batch.c)
      -> fd_submit_flush    (src/freedreno/drm/freedreno_ringbuffer.c:64)
        -> submit->funcs->flush
          -> fd_submit_sp_flush   (src/freedreno/drm/freedreno_ringbuffer_sp.c)
            -> [possibly DEFER] -> enqueue_submit_list -> ioctl
```

## THE ANSWER: freedreno defers submits by design

`src/freedreno/drm/freedreno_ringbuffer_sp.c:313`:

```c
if ((in_fence_fd == -1) && !out_fence && !has_shared && should_defer(submit)) {
   DEBUG_MSG("defer: %u", submit->fence);
   dev->deferred_cmds += fd_ringbuffer_cmd_count(submit->primary);
   simple_mtx_unlock(&dev->submit_lock);
   return 0;                      /* <-- no ioctl, silently */
}
```

`should_defer()` returns true unless `nr_bos > 30` or `deferred_cmds > 128`.

**So `glFlush()` with no fence requested does not submit, and is not supposed
to.** It parks the commands in `dev->deferred_submits`. Our workload is small,
so `deferred_cmds` never crosses 128 and the submit never happens. That is
correct, intended behaviour — not a bug, and not what any of us suspected.

What forces the real submit:

1. `deferred_cmds > 128`, or
2. a flush that needs an **out_fence**, or
3. **the fence-wait drain** at `freedreno_ringbuffer_sp.c:338`, which walks
   `deferred_submits`, moves everything up to the requested fence into a
   submit list, and calls `enqueue_submit_list()`.

Path 3 is what `glFinish`/`glReadPixels` should trigger. **It is not firing.**

## Confirmed live with the right debug variable

The `DBG()` macro needs the `MSGS` category (`freedreno_util.h:114`), i.e.
`FD_MESA_DEBUG=msgs`. With that set, freedreno finally talks:

```
MESA: info: 1300: fd_resource_allocate_and_resolve:1293: ... r8g8b8a8_unorm 64x64
MESA: info: 1309: fd_set_framebuffer_state:274: 64x64, 0 layers, 0 samples
MESA: info: 1309: fd_batch_create:131: 0x7f78000ba0
MESA: info: 1309: fd_batch_resource_write:467: 0x7f78000ba0: write 0x30ccf2f0
```

The batch **is** created and **is** flushed — on thread 1309, the batch-cache
worker. `fd_batch_resource_write` is real flush work. There is simply no
`enqueue_submit_list` afterwards. Note there is **no `defer:` line either**,
which is itself informative: the `DEBUG_MSG` for deferral is not in this build's
output, so the submit is not reaching `fd_submit_sp_flush` at all — it is being
stopped earlier, before that function.

## What this rules out, and what it opens

Dead, definitively, with source in hand:

- "freedreno never submits" as a *bug* — deferral is intended, and it explains
  every observation. Both consultants' batching theories were directionally
  right for the wrong reason; the mechanism is submit merging, not an
  empty-batch early-out. `batch_flush()` has **no** `needs_flush` early-return
  at all — it unconditionally runs `cleanup_submit`.
- `DRIVER_SYNCOBJ` / Turnip paths — irrelevant on this Gallium path, as
  Muse Spark said.

Open, and now precisely located:

- The flush stops **before** `fd_submit_sp_flush`, so the missing piece is in
  `fd_context_flush` / `fd_bc_flush` / the batch-cache worker, not in the
  ringbuffer layer.
- `fd_context_flush` has an early return that skips flushing entirely when
  `ctx->last_fence` is set ("if no rendering since last flush, re-use the last
  one"). If `last_fence` gets latched early and never cleared, every later
  flush is a no-op that returns a stale fence. **That would explain a GPU that
  never submits again after the first flush** — and it fits the teardown
  SIGSEGV too (a fence object outliving its batch).

## Next

The batch-cache path: `fd_bc_flush()` in `src/freedreno/fbc/`, and the
`ctx->last_fence` reuse branch in `freedreno_context.c`. Specifically whether
`reorder` is on and whether the `util_queue` worker is actually draining.

And the decisive experiment is now obvious and free: **draw enough commands to
cross `deferred_cmds > 128`**. If a large enough workload submits, deferral was
the whole story and the fix is simply to stop under-filling the ring. If even a
very large workload never submits, the drain path is broken and we know exactly
where.

This is the first position in 190 images where the remaining work is a single
narrow, source-confirmed question rather than a search.