# 🎯 ROOT CAUSE of the remaining wall: Mesa never creates a batch, so nothing is ever submitted

Date: 2026-11-11. Kernel `#147` (image 202). **This supersedes the IRQ theory in
`ROOT-CAUSE-GPU-IRQ-NEVER-FIRES.md`** — the IRQ is a symptom, not the cause.

## The measurement that settles it

Before and after a real `glClear` on the A640 (stages 1-3, which are known safe):

```
BEFORE:  irq=0  RB_RPTR=0x63  RB_WPTR=0x63  RBBM_STATUS3=0x0  CP_INTERRUPT_STATUS=0x0
   glClear, FBO status = 0x8CD5 COMPLETE, glGetError=0x501 (GL_INVALID_VALUE)
AFTER:   irq=0  RB_RPTR=0x63  RB_WPTR=0x63  RBBM_STATUS3=0x0  CP_INTERRUPT_STATUS=0x0
```

**The ring pointers do not move.** `0x63` is the bring-up marker (dword 99).
A `glClear` that produced no commands would leave exactly this.

And in the kernel:

```
$ dmesg | grep -c GEM_SUBMIT
0
```

**Mesa never submitted a single command.** Not one submit ioctl, ever.

## With `FD_MESA_DEBUG=msgs`, the exact stopping point

```
fd_screen_create:1093:  GPU-id: FD640
fd_screen_create:1094:  Chip-id: 0x0000000006040001
fd6_screen_is_format_supported: ... (normal probing)
fd_resource_allocate_and_resolve:1293: 0x319434a0: target=2d,
        format=r8g8b8a8_unorm, 64x64x1, ... bind=2
about to glClear
OK   glClear
=== stage 3 PASSED ===
                        <-- NO fd_batch_create. NO fd_batch_resource_write. NO submit.
```

The driver attaches to the right chip, the render target allocates, format
support queries work, `glClear` returns without error — and then **Mesa's
freedreno batch layer never produces a batch**, so there is nothing to submit.

## Why the IRQ is a symptom, not the cause

`A640` shows 0 in all eight CPU columns of `/proc/interrupts`, forever. That is
correct behaviour when nothing is ever submitted: no commands means no completion
means no interrupt. Chasing the IRQ path was a reasonable guess but wrong — the
mask is programmed correctly (`INT_0_MASK=0x03d0e242`, byte-identical to
upstream's `A6XX_INT_MASK`, verified by computing it from the register header),
the handler is registered (`msm_gpu.c:759`), and there is simply no work
completing to raise one.

## Relation to the known Mesa bug

This is the same defect recorded in `MESA-SOURCE-ANALYSIS.md` and
`DEFERTEST-RESULTS.md`, but now with the GPU actually working, which makes it the
*only* thing left:

- Those notes (pre-202-fix): `fd_batch_create` fired **once**, then
  `fd_batch_resource_write` fired, then nothing — no `enqueue_submit_list`, no
  `defer:` line. Documented question: why does the flush stop after
  `fd_batch_resource_write`, before `fd_submit_sp_flush`?
- Now (GPU genuinely up): `fd_batch_create` never fires at all.

Both are consistent with one upstream defect: the flush path that should convert a
batch into an `fd_submit` is not running. The specific place to read is
`fd_context_flush()` in Mesa 22.3.6 — particularly the `ctx->last_fence`
early-return branch that skips flushing entirely when no rendering happened since
the last flush, and the `fd_bc_flush()` batch-cache worker (which only runs when
`ctx->screen->reorder` is set).

Note the retracted theory: a previous session proposed a `batches[32]` /
`ffs()` overflow in `freedreno_batch_cache.c` and then **retracted it as wrong**
(`ffs()` is 1-based and bit 0 is never set, so `idx` is always 1 — a leak, not an
overrun). Do not resume from that.

## Why this is now the ideal place to be

Everything kernel-side is proven:

| layer | status |
|---|---|
| GMU firmware boot | ✅ 3-for-3, `BABEFACE ms~1` |
| HFI transport | ✅ 7/7 ACKs, 0 timeouts |
| ring programmed | ✅ `RB_CNTL=0x20c`, `rb_iova=0x1000` |
| CP clocking | ✅ from hardware timestamp counter |
| Mesa attaches to real chip | ✅ `FD640`, `Chip-id 0x06040001` |
| Mesa allocates resources | ✅ `r8g8b8a8_unorm 64x64` |
| **Mesa creates a batch** | ❌ **never** |
| GPU executes | ❌ no commands to execute |

For the first time the blocker is entirely in userspace, in one function's worth
of Mesa, with a source tree available at `/tmp/opencode/mesa2236` (re-clone it;
`/tmp` does not survive a reboot).

## Next step

Read Mesa 22.3.6's submit path end to end with the GPU known-good, in this order:

1. `fd_context_flush()` — the `last_fence` early-return branch.
2. `fd_bc_flush()` / `fd_batch_cache_worker` — is the batch-cache worker even
   being armed? Check `ctx->screen->reorder`.
3. `fd_batch_flush()` → `cleanup_submit()` → `fd_submit_flush()` — find where a
   created batch fails to become an `fd_submit`.
4. Instrument by building Mesa with `FD_MESA_DEBUG=msgs` and adding a print in
   `fd_batch_create`'s caller to see whether the batch layer is entered at all.

Also worth one cheap check on the kernel side: `a6xx_submit` at
`a6xx_gpu.c:1651` — verify it is actually reachable, since a NULL/short-circuited
`submit` funcs entry would look identical from userspace (no ioctl, no error).
That is a 2-minute read and would definitively split "Mesa bug" from "driver
submit never wired up".
