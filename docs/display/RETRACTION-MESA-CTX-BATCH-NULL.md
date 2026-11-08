# CORRECTION: `fd6_clear` is NOT missing batch acquisition — a common wrapper provides it

Date: 2026-11-11. This **retracts** `ROOT-CAUSE-MESA-CTX-BATCH-NULL.md` and
`MESA-BATCH-FIX-SHAPE.md` as the explanation. Do not build the proposed one-line
fix — it is unnecessary.

## What was wrong

My audit scoped `fd_context_batch()` callers to `a6xx/` and found only
`fd6_framebuffer_barrier()`. That was an incomplete search. Driver-wide:

```
freedreno_draw.c:326   struct fd_batch *batch = fd_context_batch(ctx);   (draw path)
freedreno_draw.c:336   batch = fd_context_batch(ctx);                    (draw path, retry)
freedreno_draw.c:444   struct fd_batch *batch = fd_context_batch(ctx);   (fd_clear)
freedreno_draw.c:454   batch = fd_context_batch(ctx);                    (fd_clear, retry)
freedreno_context.c:60/324  (flush paths)
```

The key one is `fd_clear()` (`freedreno_draw.c:443`), which is the common
`pipe->clear` entry point. It runs, in order:

1. `fd_render_condition_check(pctx)` — may return early
2. `fd_context_batch(ctx)` — **creates the batch if needed**
3. `batch_clear_tracking(batch, buffers)` — may flush an in-flight batch
4. `fd_batch_needs_flush(batch)`
5. `ctx->clear(ctx, buffers, ...)` → `fd6_clear()` — **batch guaranteed**

So `fd6_clear`'s dereference of `ctx->batch` is a *contract*, not a bug: it is
only ever invoked from `fd_clear`, which has already acquired a batch. In
release builds this holds identically.

## Why the observed symptom was inconsistent with the theory anyway

The consultant (Muse Spark) correctly flagged the logic problem I should have
caught myself: a genuine NULL deref of `ctx->batch->framebuffer` would produce a
**SIGSEGV**, not "no crash, no submit". The observed symptom — driver attaches,
resources allocate, clear returns `GL_INVALID_VALUE` (`0x501`), process exits
cleanly — fits "the clear path returns early or drops the batch downstream",
not a NULL deref. That was the tell, and I missed it.

## What is still unknown (the real question, unchanged)

Why does no `GEM_SUBMIT` ever reach the kernel? The candidates, ordered:

1. **`fd_render_condition_check()` returns false** → `fd_clear` exits before
   creating a batch. Would produce exactly the observed symptom.
2. **`fd6_clear()` returns `true` via the fast path but the batch is never
   flushed** — e.g. `fast_cleared` recorded but `emit_clears()` never runs
   because nothing triggers the batch's first emit.
3. **`fd_batch_needs_flush()` misfires** — the batch is marked but the deferred
   machinery (`fd_submit_sp_flush` / `enqueue_submit_list`) never drains, which
   is the original `DEFERTEST-RESULTS.md` / `MESA-SOURCE-ANALYSIS.md` question:
   *why does the flush stop after `fd_batch_resource_write`, before
   `fd_submit_sp_flush`, with no `defer:` line?*

The decisive experiment is unchanged: `FD_MESA_DEBUG=msgs` shows whether
`fd_clear`'s `DBG("%p: %x %ux%u ...")` line prints. If it does, the batch exists
and the fault is downstream (candidate 2/3). If it does not, the fault is in the
wrapper (candidate 1) — and `fd_render_condition_check` becomes the target.

## Lessons kept

- **Audit driver-wide, not directory-scoped.** The common files
  (`freedreno_draw.c`, `freedreno_state.c`, `freedreno_context.c`) hold the
  entry points; per-gen files hold the implementations. Checking only `a6xx/`
  inverted the answer.
- **Trust the symptom shape.** "No crash" rules out NULL-deref theories. State
  the predicted crash mode of every theory explicitly and check it against what
  is observed.
- The retracted `batches[32]` theory, the IRQ theory, the pm-stall theory and
  the watchdog theory are all still retracted. None of them is revived by this.
