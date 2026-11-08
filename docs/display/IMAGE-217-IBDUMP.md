# IMAGE 217 — INDIRECT BUFFER DUMP: the color render target is never programmed

Date: 2026-10-11
Image: `artifacts/flash/217-halium-boot-a6xx-ibdump.img`
Kernel sha: 69e2db37b2f8444b (ramdisk identical to 216, header identical)
Captures: `artifacts/gpu-bringup/captures/217-ibdump.txt`, `217-full.txt`, `217-preboot.txt`

## Decision rule declared BEFORE flashing

Image 217 adds a dump of the indirect buffer **contents** (not just the ring).
- If the IB contains a clear (RB_BLIT_INFO + CLEAR_MASK, or CP_BLIT) → Mesa-side bug, fix Mesa.
- If the IB contains a render pass but no color target → render target binding bug.
- If the IB is empty/garbage → the IB reference itself is wrong.

## Gate: FD640 required

The FIRST run after boot returned `GL_RENDERER = llvmpipe` and its result was
**discarded**. Cause: `a6xx.gpu_init_delay_ms=300000` holds the core unloaded;
the trigger is `echo 1 > /sys/devices/platform/soc/ae00000.qcom,mdss_mdp/a6xx_gpu_load`.
After triggering: `BABEFACE ms~1`, `hw_init ret=0`, and
`GL_RENDERER = FD640` / `GL_VENDOR = freedreno`. Only that run counts.

## Result: the clear has no color target to clear into

The kernel ring (RINGCPU) only ever showed the a6xx_submit envelope. This image
dumped the IB itself: 319 dwords at IOVA 0x24000, seq=1, cmd[0].

Decoded with the PM4 type-4/type-7 layout from `adreno_gpu.h:317-325`
(`PKT4`: cnt = bits 0-6, reg = bits 8-23; type discriminator = bits 28-29).

### What IS in the stream
- `X1_BIN_SIZE = X2_BIN_SIZE = 0x00c00000` → **BYPASS** (sysmem path), matching
  `fd6_emit_sysmem_prep` → `set_bin_size(ring,0,0,0xc00000)`.
- `RB_DEPTH_BUFFER_INFO` (6 dwords, all zero)
- `GRAS_SU_DEPTH_BUFFER_INFO`, `RB_STENCIL_INFO`, `GRAS_2D_BLIT_INFO`, all zero
- `CP_VIZ_QUERY`, `CP_SET_VISIBILITY_OVERRIDE`, `CP_WAIT_FOR_IDLE`
- 15 × `CP_EVENT_WRITE`, `CP_SET_DRAW_STATE`, `CP_SKIP_IB2_ENABLE_GLOBAL`

### What is NOT in the stream
- **`REG_A6XX_RB_MRT_BUF_INFO(0)` = 0x8822 — ZERO occurrences.**
  `RB_MRT` is an array at 0x8820 stride 8; BUF_INFO is +2. Scanned every
  packet: the 0x8800-0x8840 range contains only 0x8802/0x8804/0x880f/0x8811/
  0x8818-0x881e. 0x8822-0x8825 are absent.
- No `RB_BLIT_DST_INFO` (0x88d7), no `RB_BLIT_CLEAR_COLOR_DW0` (0x88df).

### EMIT_MRT DID RUN — its loop body did not

**Correction to an earlier reading of this file.** I first claimed
`GRAS_LRZ_MRT_BUF_INFO_0` was absent, which would have meant `emit_mrt()` was
never called. That was wrong, and the cause was a bad register table: the
kernel tree's generated `a6xx.xml.h` labels **0x8102 as
`GRAS_2D_BLIT_INFO`**, while Mesa's authoritative `a6xx.xml:1800` says
**0x8102 = `GRAS_LRZ_MRT_BUF_INFO_0`**. I had built the decoder off the kernel
header.

With Mesa's XML the corrected facts are:

- `GRAS_LRZ_MRT_BUF_INFO_0` (0x8102) **IS present, value = 0x00000000**
- `RB_SRGB_CNTL` present = 0, `SP_SRGB_CNTL` present = 0,
  `GRAS_MAX_LAYER_INDEX` present = 0

Those three are the **unconditional tail** of `emit_mrt()` (fd6_gmem.c:150-153),
so `emit_mrt()` was definitely called and definitely ran to completion.
`mrt0_format` is initialised to 0 and only assigned inside the loop
(`if (i == 0) mrt0_format = format;`). A written value of **0** therefore
proves the loop body never executed — same conclusion as before, but reached
through the value rather than through absence.

`OUT_REG` was verified to have **no zero-suppression** (`a6xx/fd6_pack.h:77`,
`__ONE_REG` writes `regs[i].value` unconditionally), so a zero write is still a
real write and this inference is sound.

## Why that proves the loop never ran

`a6xx/fd6_gmem.c:75-155` `emit_mrt()`:

```c
unsigned mrt0_format = 0;
for (i = 0; i < pfb->nr_cbufs; i++) {
   ...
   if (!pfb->cbufs[i]) continue;
   struct pipe_surface *psurf = pfb->cbufs[i];
   rsc = fd_resource(psurf->texture);
   if (!rsc->bo) continue;          /* <-- candidate */
   ...
   mrt0_format = format;
}
...
OUT_REG(ring, A6XX_GRAS_LRZ_MRT_BUF_INFO_0(.color_format = mrt0_format));
```

`emit_mrt` is called from `fd6_emit_sysmem_prep` (fd6_gmem.c:1623) via
`emit_zs(ring, pfb->zsbuf, NULL); emit_mrt(ring, pfb, NULL);`

Both ran. `emit_zs` emitted its state; `emit_mrt` emitted only its
unconditional tail with `mrt0_format == 0`, so the loop never executed. The
remaining `continue` conditions inside the loop body are:
1. `i >= pfb->nr_cbufs` — i.e. **`nr_cbufs == 0`**
2. `!pfb->cbufs[i]` — null color surface
3. `!rsc->bo` — `fd_resource(psurf->texture)->bo == NULL`

Note `emit_zs` produced all-zero depth state, which is also consistent with
`zsbuf` being set but effectively empty — the depth buffer is as unpopulated
as the color one, despite the harness clearing DEPTH too.

## Ruled out this round
- Not the "empty stream" theory: the IB is 319 dwords of well-formed work.
- Not IB units: Mesa `__OUT_IB5` also emits `bytes/4` (dwords), matching
  `submit_cmd.size / 4`.
- Not a Mesa-version mismatch: `CP_SCRATCH_REG(2)` + `CACHE_FLUSH_TS|(1<<31)`
  is kernel-side, emitted by donor v4.19 `a6xx_submit` (lines 100-108).
- Not the harness: `GL_RGBA=0x1908` correct, FBO COMPLETE, glClear/glReadPixels
  both error 0.
- Not `a6xx_submit` itself: byte-for-byte identical to donor v4.19 apart from
  the dump.

## nr_cbufs == 0 is DISPROVEN

Queried the driver directly (new diagnostic block in `glstage.py`, after the
FBO is made, before the clear):

```
MAX_COLOR_ATTACHMENTS = 8
MAX_DRAW_BUFFERS      = 8
DRAW_BUFFER           = 0x8ce0          (GL_COLOR_ATTACHMENT0)
ATTACH COLOR0 OBJ_TYPE = 36161          (0x8D41 = GL_RENDERBUFFER)
ATTACH COLOR0 RED/BLUE/ALPHA_SIZE = 8/8/8
```

The color attachment **is** bound, **is** an RGBA8 renderbuffer, and
`DRAW_BUFFER` points at it. So `pfb->nr_cbufs == 0` cannot be the cause at the
GL level, and `rsc->bo == NULL` for a bo-less renderbuffer is also unlikely.

## The submit is byte-stable across runs

All three submits are **exactly 319 dwords**. seq1 vs seq3 differ in only
**6 dwords**, and every one of them is an *address* inside query bookkeeping,
not a render target:

| dword | register/packet | seq1 | seq3 |
|---|---|---|---|
| 283 | `RB_SAMPLE_COUNT_ADDR` lo | 0x0001b010 | 0x0001f010 |
| 290 | `RB_SAMPLE_COUNT_ADDR` lo | 0x0001b020 | 0x0001f020 |
| 296 | `CP_EVENT_WRITE` 0x04 payload | 0x0001b000 | 0x0001f000 |
| 305 | `CP_EVENT_WRITE` 0x1d payload | 0x0001c000 | 0x00020000 |
| 310 | `CP_EVENT_WRITE` 0x1c payload | 0x0001c000 | 0x00020000 |
| 316 | `CP_EVENT_WRITE` 0x04 payload | 0x00012000 | 0x00015000 |

These are occlusion/query mask addresses that Mesa reallocates per frame. The
**render target address does not change and is never written** — consistent
across all three submits.

## Also absent: the whole blitter clear

`fd6_clear_surface()` (fd6_blitter.c:795-840) would emit, per color surface:
`GRAS_2D_DST_TL` (0x8405), `emit_clear_color` (`RB_2D_SRC_SOLID_C0`),
`emit_blit_setup` (`RB_BLIT_INFO` 0x88e3 / `GRAS_2D_BLIT_CNTL`), `emit_blit_dst`
(`RB_2D_DST_INFO` 0x8c17), then `CP_BLIT`.

Checked all of these against the IB: `0x8405` count 0, `0x8c17` count 0,
`0x88d7` count 0, `0x88df` count 0. **The blitter clear did not run either.**

So *neither* colour path — GMEM fast-clear nor blitter — emitted anything, while
`emit_sysmem_clears`'s unconditional depth/stencil branch did. Both colour
branches are gated on `pfb->nr_cbufs` / `pfb->cbufs[i]`, and GL reports the
attachment as present, so the divergence is **inside the batch's snapshot of
the framebuffer state**, not in what GL told us to bind.

## CONFIRMED: the batch's framebuffer state is COMPLETELY EMPTY

Verified with a strict linear PM4 walker (`ibdec2.py`, 319 dwords -> 145
packets, no overruns). Two facts nail it:

1. **`RB_DEPTH_BUFFER_INFO` (0x8872) is present with cnt=6 and all-zero
   payload** (dwords 240-247). In `emit_zs` (fd6_gmem.c:223-243) a 6-dword
   all-zero write is *exactly* the `else` branch for `zsbuf == NULL`. The
   `zsbuf != NULL` branch writes cnt=5 with a non-zero DEPTH_FORMAT.
   ⇒ **`pfb->zsbuf == NULL`.**
2. **`GRAS_LRZ_MRT_BUF_INFO_0` (0x8102) = 0, `RB_SRGB_CNTL` (0x880f) = 0,
   `SP_SRGB_CNTL` (0xa98a) = 0, `GRAS_MAX_LAYER_INDEX` (0x8004) = 0** —
   the unconditional tail of `emit_mrt()`. All four present with exact
   predicted headers (`0x40810201`, `0x48880f01`, `0x40a98a01`, `0x48800401`),
   so `emit_mrt()` ran to completion.
   ⇒ **`mrt0_format` stayed 0 ⇒ the loop body never executed.**

So at flush time: `nr_cbufs == 0` **and** `zsbuf == NULL`. Both colour loops
(`emit_mrt`, `emit_sysmem_clears`) and the depth branch all skip. One coherent
cause: **`batch->framebuffer` is empty**, even though the GL FBO reports
COMPLETE and `glGetFramebufferAttachmentParameteriv` reports a valid RGBA8
renderbuffer bound to COLOR_ATTACHMENT0.

### Correction to my own earlier decoder

My first tolerant parser reported `RB_DEPTH_BUFFER_INFO` as ABSENT, which
would have implied `emit_zs` ran a branch it did not run. It was a parser
desync — the header `0x48887286` decodes as type-4 reg=0x8872 cnt=6. I had also
built the register table off the kernel's generated header, which mislabels
`0x8102` as `GRAS_2D_BLIT_INFO`. Both are fixed in `ibdec2.py`, which uses
Mesa's authoritative XML and asserts on overrun. Lesson: for this stream,
only the strict walker is trustworthy.

### Correction: the RB_BLIT_* absence was a red herring

I earlier treated missing `RB_BLIT_DST_INFO` (0x88d7) / `RB_BLIT_CLEAR_COLOR_DW0`
(0x88df) as significant. They are **not** the bypass clear path.
`emit_sysmem_clears` -> `fd6_clear_surface` uses the **2D blitter** register
set: `GRAS_2D_DST_TL` (0x8405), `RB_2D_SRC_SOLID_C0` (0x8c2c),
`RB_2D_BLIT_CNTL` (0x8c00), `GRAS_2D_BLIT_CNTL` (0x8400), `RB_2D_DST_INFO`
(0x8c17), `CP_BLIT`. The `RB_BLIT_*` family is GMEM-path only (`emit_blit`,
`emit_clears`, `emit_restore_blit`), reachable solely via
`fd6_emit_tile_renderprep`. All of the 2D-blitter registers are absent too, so
the conclusion is unchanged, but the reasoning had to be redone.

## IMAGE 218: the image 217 conclusion was based on a STALE submit

### The measurement bug

Image 217 gated every diagnostic on `submit->seqno <= 3`. After the first three
submits the dump goes **permanently silent**. Discovered by counting:

```
BEFORE a fresh glstage run:  966 "IBDUMP[seq" lines
AFTER  a fresh glstage run:  966   (unchanged)
```

So *no new submit produced any output at all*. The 319-dword IB analysed
throughout this document was captured from the **first** run, and every
subsequent run — including every one run while investigating — was invisible.

That is how a stale capture came to be read as "the glClear stream". The
`seqno <= 3` gate was correct when the only thing we ran was the first submit;
it became actively misleading once we started iterating.

Image 218 replaces the gate with a runtime module parameter (`ibdump`, default
**off**, so zero cost and no log flood) and gates the `RINGCPU` and `CPINT`
dumps on it too, since they suffer the identical problem. Enable it around a
run, disable before reboot.

### What the real clear batch looks like (userspace, no reflash needed)

`FD_MESA_DEBUG=msgs` on the current (217) device:

```
fd_set_framebuffer_state:274: 64x64, 0 layers, 0 samples
fd_batch_create:131: 0x7f44000ba0
fd_clear:466: 0x7f44000ba0: 4 64x64 depth=1.000000, stencil=0 (r8g8b8a8_unorm/none)
gmem_stateobj_init:293: using 1 bins of size 64x64
```

This directly **contradicts** the "empty framebuffer" conclusion drawn from the
stale IB. The clear batch has:
- `cbufs[0]` = **r8g8b8a8_unorm** (non-NULL — `fd_clear`'s DBG line calls
  `pipe_surface_format(pfb->cbufs[0])`, which would crash on NULL)
- `buffers = 4` = `PIPE_CLEAR_COLOR`, and depth/stencil correctly `none`
- `gmem_stateobj_init: using 1 bins of size 64x64` ⇒ the batch took the **GMEM**
  path, not sysmem/bypass

So `fd6_clear()` is reached, records the clear, and the framebuffer is fully
populated. The earlier reasoning about `nr_cbufs == 0` and `zsbuf == NULL` was
an artifact of analysing the wrong submit.

### Also visible in the same log

`handle_rgba_blit: 2d r8g8b8a8_unorm **UBWC** -> linear` — the `glReadPixels`
readback is a blit from a **compressed (UBWC)** source buffer. So the FBO is
UBWC-compressed, which is worth noting independently: a clear into UBWC and a
readback out of UBWC are two more places the round trip can fail, and this is a
*second* candidate for the black pixel, distinct from "the clear never ran".

### Correction to my own claim about GL_OUT_OF_MEMORY

I reported the texture-attachment A/B failing with `0x501` as
`GL_OUT_OF_MEMORY`. That is wrong: `0x501` is **`GL_INVALID_VALUE`**;
`GL_OUT_OF_MEMORY` is `0x505`. There is no evidence of an allocation failure,
and I should not have built a theory on it.

### Net state

Unresolved, but the question is now much sharper and the measurement is
trustworthy again. Next step is image 218 + `echo 1 > /sys/module/a6xx/parameters/ibdump`
around a single run, which will finally capture the *actual* clear submit.