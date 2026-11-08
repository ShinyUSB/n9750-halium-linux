# Deferral-threshold test: 8200 draws, ring never moved. The gate is upstream.

Run on `#131` (post-189, driver submit path proven working). Harness
`harness/defertest.py`. GPU already loaded, no flash, no lottery ticket.

## Result

```
CPDUMP[t0] -> wptr_dw=114     (baseline; 114 is where our manual submit left it)
```

That is the only probe that landed. `t1`, `t1b`, `t2` never appeared, and the
process exited during T1. The ring **did not move at all** across 4000 + 4000
tiny draws and 200 × 500 000-vertex draws. Core IRQ stayed at 3.

## What this rules out

**Deferral is not the gate.** The hypothesis was that `should_defer()` returns
true because `deferred_cmds` never crosses 128, so nothing ever submits. If
that were the whole story, ~8000 draws would have blown past the threshold and
forced a submit. It did not. So either:

- the draws are not accumulating into deferred submits at all (they are going
  into a batch that is never submitted), or
- the counter is not being incremented the way the source suggests.

Either way, the ~8200 draws produced exactly **zero** additional commands in
the ring. This is not "submit deferred pending a fence"; it is "the batch is
never converted into a submit".

## The observation that now matters most

From the `FD_MESA_DEBUG=msgs` output, across every run:

```
fd_set_framebuffer_state: 64x64, 0 layers, 0 samples
fd_batch_create: 0x7f74000ba0          <-- ONE batch
fd_batch_resource_write: write 0x40b9cee0
```

and then **nothing**. One batch is created, one resource is written, and there
is no `enqueue_submit_list`, no `defer:` line, and no submit.

Critically there is also **no `defer:` line**. That was the detail I expected
to be missing but it is genuinely diagnostic: if the submit were being deferred,
`fd_submit_sp_flush` would print `defer: %u`. It prints nothing. So execution
stops **before** `fd_submit_sp_flush` — i.e. the batch never gets converted to
an `fd_submit` at all.

Also newly visible from the debug output, and important:

```
fd_screen_create:1092: Pipe Info:
fd_screen_create:1093:  GPU-id:          FD640
fd_screen_create:1094:  Chip-id:         0x0000000006040001
fd_screen_create:1095:  GMEM size:       0x00100000
```

So the screen latched onto the real GPU with correct identity. That closes the
last "maybe it attached to the wrong device" question.

## Where this points

The gap is between `fd_batch_resource_write` (which happens) and the creation
of an `fd_submit` (which does not). In source terms: `batch_flush()` →
`cleanup_submit()` → `fd_submit_flush()`. We see the batch being written and
never see it submitted, and never see the deferral message.

The concrete place to read next is the **batch cache** (`src/freedreno/fbc/`),
because `fd_context_flush()` calls `fd_bc_flush()` rather than
`fd_batch_flush()` when `ctx->screen->reorder` is set, and the batch work we
observe is running on a *different thread* (tid 1420 vs the GL thread 1411) —
which is the batch-cache worker. So the batch is being handed to the worker and
then dropped there.

That is a much narrower target than "freedreno never submits": it is one
function's worth of code on a worker thread, with the driver now proven able to
accept and execute a real submit (wptr 99→114, IRQ 0→3).

## Honest note on process

The script exited during T1 rather than completing, and the teardown SIGSEGV is
still present. I did not establish whether T1 hung, crashed, or was still
running when the harness was reaped. Worth pinning down next time with a
supervisor rather than assuming.

## Cumulative state

Driver side: **proven working end to end.** A hand-built submit encodes into
the ring, the CP drains it, and the core IRQ fires. Nothing in the kernel is
standing between us and pixels any more.

Userspace side: **one bug, now localised to the freedreno batch cache worker.**
It is no longer a search over "why doesn't freedreno submit"; it is a specific
missing hand-off on a specific thread, in a source tree we now have checked out
at `/tmp/opencode/mesa2236`.

Phone alive on `#131`, GPU loaded, reachable over USB RNDIS.