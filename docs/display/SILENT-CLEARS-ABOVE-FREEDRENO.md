# Silent clears: Mesa swallows them with no batch, no DBG, no error

Date: 2026-11-11. Mesa 22.3.6 on the phone. Kernel `#147` (image 202), 43 min
uptime, GPU still clocking.

## The measurement

Three `glClear`s with changing viewport, scissor and clear colour:

```
FBO status=0x8cd5            (COMPLETE)
RENDERER=FD640
clear[0] vp=(0,0,64,64) err=0x0
clear[1] vp=(0,0,32,32) err=0x0
clear[2] vp=(8,8,48,48) err=0x0
```

All succeed with `glGetError() == 0`. And in `FD_MESA_DEBUG=msgs`:

```
fd_screen_create ... FD640 ... Chip-id 0x06040001
fd_resource_allocate_and_resolve: ... r8g8b8a8_unorm, 64x64 ...
                                          <-- then NOTHING
```

No `fd_set_framebuffer_state` DBG (`%ux%u, %u layers`). No `fd_clear` DBG
(`%p: %x %ux%u`). No `fd_batch_create`. Ring frozen at the bring-up marker:

```
CP_RB_WPTR 0x00000063 / CP_RB_RPTR 0x00000063
```

Zero `GEM_SUBMIT` ever.

## What this proves

The clears are swallowed **before** the freedreno driver layer is even entered.
The `fd_clear()` wrapper in `freedreno_draw.c` (which would print its DBG line
and call `fd_context_batch()`) is never reached. Neither is
`fd_set_framebuffer_state()` (which would print its `%ux%u layers` line). So the
fault is **above** freedreno — in the Mesa state tracker or the threaded-context
(glthread) layer — not in `fd6_clear`.

Concretely, the path is:

```
app glClear
  -> [glthread queues tc_clear]
  -> tc_call_clear: pipe->clear(pipe, ...)        u_threaded_context.c:4317
     -> fd_clear()                                 freedreno_draw.c:443
        -> DBG(...)                                NEVER PRINTS
```

Something between the app call and `pipe->clear` drops it. The candidates:

1. **glthread never drains the queued clear.** `tc_add_call(tc, TC_CALL_clear,
   ...)` buffers the call for the driver thread. If the driver thread never
   runs it, the clear vanishes silently. The threaded context exists because
   `tc_assert_driver_thread` is asserted on, so it is active — but "active" and
   "draining" are different things.
2. **`st_validate_state` short-circuits.** `st_Clear` calls
   `st_validate_state(st, ST_PIPELINE_CLEAR)` (st_cb_clear.c:432), which calls
   `st_manager_validate_framebuffers` → `st_context_validate`, which dirties
   `ST_NEW_FRAMEBUFFER` only when the stamp changes. On a *repeat* clear with an
   already-validated FBO, the stamp is unchanged — but that explains clears 2-3,
   not clear 0, whose stamp must differ from the default framebuffer.
3. **The degenerate-framebuffer early-out.** If the state tracker considers the
   draw framebuffer empty/incomplete (e.g. `nr_cbufs == 0` because
   `rb->surface` is NULL for our renderbuffer), it may skip the driver call
   entirely. `glCheckFramebufferStatus == COMPLETE` argues against this, but
   COMPLETE is the GL-level state, not the pipe-level one.

Clear 0 is the important one: a first clear on a freshly-bound FBO, where the
stamp must change and the framebuffer is complete, yet the driver's
`set_framebuffer_state` DBG never prints. That eliminates theory 2 as the whole
story.

## Next experiment (cheap, no build)

Force the threaded-context drain explicitly: set `MESA_NO_THREAD=1`
(disables glthread; the clear runs synchronously on the app thread) and re-run
the same three clears. Two outcomes:

- **Batch DBG lines appear** → glthread is the swallower. The fix is in the
  threaded-context configuration on this platform (or just keep `MESA_NO_THREAD=1`
  for the bring-up path and revisit later).
- **Still nothing** → the fault is in the state tracker (`st_Clear` →
  `st_validate_state` → stamp/CSO logic). Then instrument
  `st_context_validate`'s stamp comparison and the CSO `set_framebuffer_state`
  dispatch.

`MESA_NO_THREAD=1` is a single environment variable and decides between the two
biggest remaining code regions. Run it next.

## Standing facts (re-verified this session)

- GPU up 43 min, clocking, zero resets, zero faults.
- `GL_RENDERER=FD640`, `GL_VENDOR=freedreno`, `GL_VERSION=4.2 Mesa 22.3.6`.
- `msm_dri.so` on the phone is a RELEASE build (`#ifdef DEBUG` strings absent,
  but DBG lines compiled in — which is why their absence is evidence).
- Upstream verification of `fd6_clear` is BLOCKED (no network egress to GitHub
  from the sandbox; GitLab behind bot wall). The consultant's challenge was
  still valuable: it exposed my directory-scoped audit error and produced the
  retraction, which is how we got here.
- Kernel submit path (`a6xx_submit` at `a6xx_gpu.c:1651` via `msm_gpu.c:618`)
  verified wired and correct. Nothing to fix there.
