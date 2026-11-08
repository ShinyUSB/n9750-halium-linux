# FINDING: the CP consumes the ring; the render target is correctly referenced; the RB writes nothing

> **PARTLY SUPERSEDED — see `ROOT-CAUSE-SQE-NEVER-STARTS.md`.**
> Section 1 below ("the CP consumed the ring - CONFIRMED") is **WRONG and is
> retracted**. `CP_RB_RPTR` is advanced by the ROQ DMA prefetcher, a different
> clock domain from the SQE, so RPTR tracking WPTR proves the prefetcher
> drained the ring, NOT that the SQE executed anything. The SQE is frozen at
> PC `0x12e` across every submit.
> Sections 2-6 remain sound: debugfs is unusable, the render target is
> correctly identified and referenced, the clear colour is emitted correctly,
> and the render-path matrix is valid. Those are userspace-side facts.

Date: 2026-11-16. Follows `FINDING-SUBMISSION-IS-COMPLETE.md`.
Harnesses: `harnesses/cpstate_poller.py`, `harnesses/ringprobe.py`,
`harnesses/ubwc_matrix.sh`.

## 1. The CP consumed the ring — CONFIRMED

`a6xx_gpu_cpstate` (the image-196 readable node) sampled at ~80 Hz for 40 s,
**fsync'd after every sample** so a hard reset cannot destroy the evidence.

5054 valid samples. `CP_RB_RPTR` and `CP_RB_WPTR` tracked each other exactly:

```
t= 0.0025  RPTR=0x000000cc  WPTR=0x000000cc
t= 2.0470  RPTR=0x000000db  WPTR=0x000000db
```

RPTR advanced `0xcc -> 0xdb` (+15 dwords) and caught WPTR. Throughout:

| signal | value | meaning |
|---|---|---|
| `CP_RB_RPTR` | advanced to WPTR | CP fetched the whole ring |
| `PERFCTR_CP_0` | `clocking 1` on 5054/5054 | CP genuinely fetching, not idle |
| `CP_INTERRUPT_STATUS` | `0x00000000` | no fault |
| `RBBM_STATUS` | `0x00000001` | bit 0 = `CP_AHB_BUSY_CX_MASTER`, a busy flag |

`RBBM_STATUS` bit 0 is **not** a halt. The tree documents this at
`a6xx_gpu.c:1404`. Do not read `0x1` as "RB stopped".

So the ring handoff works. The submission is byte-correct
(`FINDING-SUBMISSION-IS-COMPLETE.md`). Together: userspace builds a correct
command stream and the hardware consumes it.

## 2. debugfs `rd` is unusable here — confirmed, not assumed

```
$ cat /sys/kernel/debug/dri/0/rd      -> whitespace only
$ echo 1 > /sys/kernel/debug/dri/0/gpu -> REJECTED (file is -r--r--r--)
```

This confirms the existing note at `msm_drv.c:1220-1222` ("msm's debugfs
register interface is non-functional in this CAF tree"). `a6xx_gpu_cpstate`
remains the only register route, so everything below rests on it.

## 3. The render target, identified by role

A size sweep, clear-only, every capture self-verified (`headers == nr_bos`):

| W x H | submitted BO sizes | area*4 |
|---|---|---|
| 32x32 | 4x 4096 | 4096 |
| 64x64 | 4x 4096, **16384**, 32768 | 16384 |
| 128x128 | 4x 4096 | 65536 |

At 64x64 there is exactly one 16384-byte BO = 64*64*4 = a linear RGBA8 colour
buffer. Under `FD_MESA_DEBUG=sysmem` it is still present (BO set drops from 6
to 5 and the 32768 GMEM buffer disappears, so the switch is honoured).

**And it is correctly referenced.** Relocation containment check over the
sysmem submission:

```
cmdstream dw 237 -> 0x700025000  INSIDE the 16384 RT (+0x0)
cmdstream dw 303 -> 0x700025000  INSIDE the 16384 RT (+0x0)
```

So the RT address reaches the command stream twice. The RT is not orphaned.

## 4. The clear colour is emitted correctly

In the sysmem command stream:

```
[222] 0x488c2c04   header, reg 0x0c04 = RB_BLIT_CLEAR_COLOR_DW0
[223] 0x00000000   DW0
[224] 0x00000000   DW1
[225] 0x000000ff   DW2  (B = 255)
[226] 0x000000ff   DW3  (A = 255)
```

for `glClearColor(0,0,1,1)`. Mesa emits the clear with the right value.

## 5. Render-path matrix — every path fails

| `FD_MESA_DEBUG` | capture | clear landed? |
|---|---|---|
| (default) | complete | no |
| `noubwc` | complete | no |
| `sysmem` | complete | no |
| `nobin` | complete | no |
| `notile` | complete | no |
| `noubwc,sysmem` | complete | no |

The driver really does accept these: `FD_MESA_DEBUG`, `noubwc`, `notile`,
`nobin`, `sysmem`, `inorder`, `flush` are all present as literal strings in
`/usr/lib/aarch64-linux-gnu/dri/msm_dri.so`. So UBWC compression and the
tiled/GMEM path are **ruled out** as the cause.

## 6. Corrections to claims I made mid-session

**(a) "The clear colour appears in no submitted BO, so the RB writes nothing."
Only valid for sysmem.** In the default tiled path `emit_clears()`
(`fd6_gmem.c:1049-1113`) clears into **GMEM**, not into a submitted buffer:

```c
OUT_RING(ring, A6XX_RB_BLIT_INFO_GMEM | A6XX_RB_BLIT_INFO_CLEAR_MASK(0xf));
OUT_PKT4(ring, REG_A6XX_RB_BLIT_BASE_GMEM, 1);
OUT_RING(ring, gmem->cbuf_base[i]);
```

GMEM is an on-chip carveout, so its absence from `bos[]` is expected and
proves nothing. I had the instrument pointed at the wrong place.

**(b) The colour search had to cover component-swapped patterns.**
`fd6_color_swap()` reorders RGBA per format, so `0xffff0000` alone is not a
sufficient pattern. In sysmem the value appears as per-component dwords
(`0x000000ff`), which is what actually confirmed emission.

**(c) Unresolved anomaly: the RT does not scale with area.** At 128x128 no
65536-byte BO appears at all, only 4x4096 — the same set as 32x32. Either
`gbm_surface_create` failed and EGL fell back, or the RT is not where assumed.
**`ringprobe.py` does not check `surf != NULL`**, so this is an instrument gap,
not yet a finding. Fix before drawing any conclusion from 128x128.

## Where this leaves the bug

Ruled out: command-stream construction, relocation, BO identity, buffer
bookkeeping, ring handoff, CP execution, UBWC, tiling/GMEM path, binning.

Everything userspace does is correct and the hardware consumes it. The RT is
addressed, mapped, and the clear value is present in the stream — yet the RT
stays all-zero. That points at **RB/GRAS execution of the blit/clear**, which
no current instrument observes.

## Proposed next step (build required — zero-build options are exhausted)

Prove whether the RB can write DDR at all, using the raw-ctypes submit path
that is already proven to work (`fullpath.py`: a 128-byte write comes back
32/32 through `DRM_IOCTL_MSM_GEM_SUBMIT`). That harness proved **the CP** can
write. The analogous test for **the RB** is:

1. Allocate a raw BO.
2. Hand-build a minimal `RB_BLIT`-based clear:
   `RB_BLIT_DST_INFO` (TILE6_LINEAR, RGBA8) + `RB_BLIT_SRC_INFO` +
   `RB_BLIT_BASE_SRC`/`BASE_DST` + `GRAS_2D_DST_TL/BR` + the
   `RB_DBG_ECO_CNTL` blit magic + `CP_WAIT_FOR_IDLE` + `CACHE_FLUSH_TS`.
3. Submit, then CPU-read the destination through the same direct mapping.

  * Destination gets the colour -> RB works; the bug is in what Mesa emits.
  * Destination stays zero -> the RB cannot write DDR, and every userspace
    avenue is a dead end.

Either answer halves the remaining search space. A cheaper variant that needs
no packet-layout guessing: extend `a6xx_cp_state_read()` to also print the
`GRAS_2D_*` / `RB_*` status registers so we can see whether the RB was armed
at all — but that is also a build.

**Do not hand-craft PM4 without reading the register header first.** That
approach has already produced two fabricated false claims in this project.