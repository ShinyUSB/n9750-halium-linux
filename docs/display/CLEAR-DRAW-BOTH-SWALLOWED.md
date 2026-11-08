# Both clear AND draw vanish silently with err=0x0 — the fault is above freedreno

Date: 2026-11-11. Mesa 22.3.6, kernel `#147`, phone up 45 min, GPU clocking.

## What was tested

A complete triangle pipeline on the A640, surfaceless GBM:

```
FBO=0x8cd5                    (COMPLETE)
RENDERER=FD640
shader VS compile=1
shader FS compile=1           (needed explicit #version 100)
link=1
about to glDrawArrays
OK glDrawArrays err=0x0
```

Shaders compile, link succeeds, the draw returns clean — and then **nothing**:
zero `fd_batch` lines, zero `GEM_SUBMIT`, ring frozen at `0x63`.

## Combined with the clear tests

| path | GL calls | returns | batch activity | submits |
|---|---|---|---|---|
| clear ×3 (varied vp/scissor) | `0x0` | none | 0 |
| draw (compiled+linked shaders) | `0x0` | none | 0 |

Both paths die at the same layer: **above** the freedreno driver. `fd_clear()`'s
DBG never prints, `fd_set_framebuffer_state()`'s DBG never prints,
`fd_batch_create` never runs. `MESA_NO_THREAD=1` changes nothing, so it is not
glthread. `XDG_RUNTIME_DIR` fixed, GBM platform works, EGL 1.4 initializes.

## Where the choke point must be

Common to both paths, below the app, above freedreno:

```
app glClear / glDrawArrays
  -> Mesa state tracker (st_Clear / st_draw, ST_PIPELINE_*)
     -> st_validate_state -> st_manager_validate_framebuffers
        -> st_context_validate (stamp check -> ST_NEW_FRAMEBUFFER?)
           -> cso_set_framebuffer (memcmp -> set_framebuffer_state?)
              -> fd_set_framebuffer_state           NEVER REACHED
```

The stamp/CSO logic is the immediate suspect set:

- `st_context_validate` dirties `ST_NEW_FRAMEBUFFER` only when `stdraw->stamp !=
  st->draw_stamp`. If the stamp never changes (e.g. the draw buffer was never
  registered as a *new* framebuffer after `eglMakeCurrent` with
  `EGL_NO_SURFACE`), the state tracker considers the framebuffer already valid
  and never forwards it.
- `cso_set_framebuffer` forwards only on `memcmp` difference. If the pipe
  framebuffer struct it builds is byte-identical to the cached one (e.g.
  zero-sized because `rb->surface` is NULL for our renderbuffer, or because the
  surfaceless default framebuffer was never populated), nothing is forwarded.
- Either way the driver never sees a framebuffer, so `fd_batch_from_fb` is never
  called with a real framebuffer, so no batch key exists, so clears and draws
  are recorded against nothing and dropped.

Notably `st_Clear` calls `st_validate_state(st, ST_PIPELINE_CLEAR)`, and the
clear path requires the *draw* framebuffer to be meaningful. With
`EGL_NO_SURFACE` there is no default framebuffer at all — everything depends on
the FBO we bound. If any step in
`glBindFramebuffer → _mesa_update_renderbuffer_surface →
st_framebuffer_validate → st_context_validate → cso_set_framebuffer`
fails to register our 64x64 renderbuffer as a real surface, the whole pipeline
silently no-ops.

## The single most informative next measurement

Stop guessing at layers and instrument the boundary. Two options, cheapest first:

1. **`MESA_LOG` + `GALLIUM_DDEBUG`.** `GALLIUM_DDEBUG` dumps every pipe call the
   driver receives. If `set_framebuffer_state` never appears in its output, the
   state tracker is confirmed as the swallower; if it appears, freedreno is.
   One env var, no build.
2. **Print the pipe framebuffer struct.** Add one `fprintf(stderr, ...)` to
   `st_update_framebuffer_state` (or read it via `apitrace`) showing
   `framebuffer.width/height/nr_cbufs` and whether `cbufs[0]` is NULL. If
   `nr_cbufs == 0`, the renderbuffer surface was never created and that is the
   bug — one level up, in surface allocation, not in state caching.

Option 1 first: it is free and it splits "state tracker" from "driver" with one
run.

## Standing facts

- GPU up 45 min, clocking, zero faults, zero resets. Nothing wrong kernel-side.
- `GL_RENDERER=FD640`, shaders compile+link, draws return `0x0`.
- Zero `GEM_SUBMIT` on every path. The ring has never moved past the bring-up
  marker `0x63`.
- `DECISIVE-CPU-LOAD-TEST.md` stands: sustained load avg 56 does not kill it.
- Fragment shaders on this profile need an explicit `#version 100` line, or
  compile fails with `0:1(1): error: syntax error, unexpected NEW_IDENTIFIER`.
  Vertex shaders compile either way.
