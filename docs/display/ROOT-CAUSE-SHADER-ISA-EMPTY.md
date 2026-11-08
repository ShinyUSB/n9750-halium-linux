# ROOT CAUSE FOUND: the shader ISA buffer handed to the SP is empty

Session of image 248. This is the first result that explains the rendering
failure at the data level rather than by elimination.

## The finding

`fd6_emit_shader()` (`freedreno/a6xx/fd6_program.c:129-133`) tells the shader
processor where its code lives:

```c
OUT_PKT4(ring, first_exec_offset, 7);
OUT_RING(ring, 0);                    /* SP_xS_OBJ_FIRST_EXEC_OFFSET */
OUT_RELOC(ring, so->bo, 0, 0, 0);     /* SP_xS_OBJ_START_LO  <- ISA buffer */
```

`so->bo` is created and filled in `ir3/ir3_gallium.c:102-116`:

```c
v->bo = fd_bo_new(compiler->dev, v->info.size, FD_BO_NOMAP, ...);
fd_bo_upload(v->bo, v->bin, 0, v->info.size);
```

so the buffer's size is exactly the compiled ISA size.

### Measured: that buffer scales with the shader and is 100% zero

Command stream dumped via the in-tree `ibdump` instrumentation, then the buffer
reference graph recovered by locating each BO's 35-bit IOVA as a 64-bit LO/HI
pair.

**Trivial shader** (`gl_Position = vec4(0,0,0,1)`, `o = vec4(1,0,0,1)`):

| BO | iova | dwords | nonzero |
|---|---|---|---|
| bo[1] | 0x700025000 | 1610 | **0** |
| bo[2] | 0x700038000 | 1024 | **0** |
| bo[3] | 0x70002e000 | 1024 | 72 (command stream) |
| bo[5] | 0x70002f000 | 1024 | 149 (command stream) |

**Deliberately large fragment shader** (120-term `x +=` chain, so the ISA is
unmistakable):

| BO | iova | dwords | nonzero |
|---|---|---|---|
| bo[1] | 0x700025000 | **5120** (20 KB) | **0** |
| bo[2] | 0x700038000 | 1024 | **0** |
| bo[3] | 0x70002e000 | 1024 | 72 (command stream) |
| bo[11] | 0x70002c000 | 1024 | 244 (command stream) |

`bo[1]` grew from 1610 to 5120 dwords in proportion to the larger shader — **it
is the ISA buffer** — and it is **entirely zero in both runs**. The command
stream points at it correctly (`bo[3][45]` and `IB[243]` carry its IOVA as a
64-bit pair).

So: Mesa compiles (`compile ok=1`), links (`link ok=1`), allocates the right
size, and emits the right address — **and the shader bytes never reach the
GPU-visible buffer.** The SP is pointed at zeros, so every draw executes a
no-op.

That single defect explains the whole signature:

- renders return `[0,0,0,0]`
- compute → SSBO returns all zeros
- **no fault, no interrupt, no GMU error** — a zero ISA is a no-op, not an error
- CP domain completely healthy (it is not executing the ISA; the SP is)
- immune to clocks, hwcg, secure mode / zap, and PM state

It also explains why the *draws* were dead regardless of which write path was
tested, and why `wptr` kept advancing: the stream is fine, the payload is not.

## What this does NOT explain — stated plainly

**`glClear` is not a draw.** A tile clear is performed by the RB/CCU with no
shader involvement. Our tests clear to opaque blue and then read back
`[0,0,0,0]`. An empty ISA cannot explain that, and buffer identity was already
proven sound (`BUFFER-IDENTITY-PROVEN.md`).

So there are **at least two defects**:

1. **Shader ISA buffer is empty** (proven above) — kills all draws.
2. **Even a shader-free clear produces nothing** — unexplained, in the
   RB/CCU/render-target path, independent of (1).

(2) is why I am not declaring victory. A fix for (1) should be immediately
visible via the draw tests, but the clear test will still fail until (2) is
resolved.

## Why the upload is lost — where to look next

The differentiator is sharp: command-stream BOs (bo[3], bo[5], bo[11]) written
through the *same* Mesa upload path contain **correct data**. Only the
`FD_BO_NOMAP` shader BO is empty. So the fault is specific to that buffer class:

- `fd_bo_upload()` on a `FD_BO_NOMAP` buffer (`freedreno_resource.c`), or
- the kernel side beneath it: `msm_gem_object_map()` / the writeback of a
  never-persistent, CPU-mapped BO to its GPU-visible pages.

Best next probes, in order of cost:

1. Read `bo[1]` back from **userspace** (Mesa/GEM) at the same moment as the
   kernel's `BOS[]` dump. If userspace sees the ISA and the kernel sees zeros,
   the bug is in the kernel mapping/writeback. If userspace also sees zeros,
   it is `fd_bo_upload` and the Mesa `NOMAP` path.
2. Add a kernel-side print of `msm_gem` `bo->mapped` / whether the object is
   flagged CPU-mapped at submit time.
3. Check whether the ISA BO is being allocated as *uncached/mapped* while the
   command BOs go through the pipelined, unmapped+explicit-transfer path.

## Supporting state established this session (all verified, not assumed)

- **zap firmware**: `a640_zap.{mdt,b00,b01,b02}` load and RUN
  (`a640_zap: Brought out of reset`, `pil-a640_zap: active_count(1)`). Built via
  a new runtime `fw_load` attribute; also fixed a real bug where
  `subsys_desc.fw_name` was never populated (see `ZAP-REAL-MECHANISM.md`).
  **Zap is not the cause** — compute still returned zeros with it loaded.
- **Clock gates**: 43/46 hwcg entries read back exactly as CAF's a640 table
  specifies, zero `0xdeadbead`, so RB/CCU/UCHE are clocked and every address is
  valid (see `IMAGE-245-RESULTS.md`).
- **PM**: `pm_hold` holds a permanent runtime reference, so autosuspend cannot
  fire. Verified `active +=25006 ms` over 25 s wall, `suspended +=0`.
  Note `runtime_status` can still read `suspending` via a *direct* suspend
  while the hardware stays fully on (`SPTPRAC_PWR_CLK_STATUS=0`,
  `RPMH_POWER_STATE=0`, ring drained, `CP_TIMESTAMP clocking 1`) — so
  `runtime_status` alone is no longer a reliable health signal; use the
  cpstate registers.