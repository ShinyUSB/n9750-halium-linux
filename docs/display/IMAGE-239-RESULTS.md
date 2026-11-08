# Image 239 — RB engages (RB=1), backend runs, still zero output

Image: `artifacts/flash/239-halium-boot-a6xx-rbobs2.img`
sha256 `84c3dc73119d3961142cfaf388a1b2b9d3dbfc9b1b5d8320e06f00b837ff88cc`
Kernel `#46`, boot `380184a9`.

## The measurement (v2, after v1's design flaw)

Image 238 sampled STATUS 5x starting 10 ms after the kick. A 64x64 clear
executes in microseconds, so it could only ever catch idle — all-idle plus
zero pixels is two states, not a finding. Discarded.

Image 239 polls STATUS in a tight loop from the kick until the GPU's own fence
echo, keeping a sticky mask of every busy bit seen during execution:

```
RBOBS2[seq=1] iters=37 fence=1 STATUS_seen=0x00e71d97 RB=1 SP=1 HLSQ=1 VPC=1 UCHE=1 CCU=1 CP=1 PC_DCALL=1
RBOBS2[seq=2] iters=7  fence=2 STATUS_seen=0x00ef15f7 RB=1 SP=1 HLSQ=1 VPC=1 UCHE=1 CCU=1 CP=1 PC_DCALL=1
RBOBS2[seq=3] iters=9  fence=3 STATUS_seen=0x00ef9df7 RB=1 SP=1 HLSQ=1 VPC=1 UCHE=1 CCU=1 CP=1 PC_DCALL=1
```

**RB=1. The backend receives the blit, engages every block including RB/UCHE/
CCU, and retires in 7-37 iterations.** Dispatch theories are dead.

## The buffer is never touched (prefill test)

`/root/prefill.py`: render red + swap, lock front, fill with `0xAA` from CPU,
release, render green + swap, lock and read from CPU memory.

```
dmabuf identity: fd 7 ino 28622 vs fd 8 ino 28622
round2 swapped; locked fd 8 (SAME buffer)
untouched 0xAA : 4096 / 4096
zeroed         : 0 / 4096
green          : 0 / 4096
VERDICT: GPU NEVER TOUCHED THE BUFFER
```

(Side lesson: fd numbers differ per lock — dma-buf identity must be compared
by inode, not fd. The first run of this test reported INCONCLUSIVE on exactly
that mistake.)

Not green (clear did not land), not zeroed (no wrong-colour clear, no zeroing
invalidate path) — the 0xAA pattern is fully intact. Combined with RB=1: the
backend runs the blit to completion and emits no memory traffic at all.

## The command stream is valid (decoded field-by-field)

Blit sequence (seq=16 bo[2]), matching Mesa 22.3.6 `fd6_clear_surface()` /
`emit_blit_buffer()` byte-for-byte:

```
[042] = 0x48840502  GRAS_2D_DST_TL, cnt 2
[043] = 0x00000000  TL X=0 Y=0
[044] = 0x00000fff  BR X=4095 Y=0 (linear buffer blit, w=4096)
[045] = 0x70460001  CP_EVENT_WRITE
[046] = 0x0000003f
[047] = 0x70268000  WFI (CP_WAIT_FOR_IDLE)
[048] = 0x408e0401  RB_DBG_ECO_CNTL <- 0x04100000 (blit magic)
[050] = 0x702c0001  CP_BLIT cnt 1
[051] = 0x00000003  BLIT_OP_SCALE
[052] = 0x70268000  WFI
```

Two things that looked like findings and were not:

- "No CP_BLIT in the stream" — a grep artifact. Blits exist in 10 submits
  (seq 16-24); the first search ran before any submit on that boot.
- "Two different dest addresses (0x700026000 vs 0x700025000)" — the UBWC
  colour/metadata plane split, not a bug. MRT colour base = target + 0x1000,
  flags = target base. Correct for 64x64 UBWC (0x4000 colour + 0x1000 meta).

Also verified: surface-clear rectangles `0x003f003f` present (26x), MRT format
`0x30` (RGBA8) + tile 3 correct, relocs patched with high word `0x7`, CP
executes past the blit (later event-writes land), translation is a real SMMU,
no faults of any kind.

## Standing

The break is strictly between "backend retires the blit" and "bytes land in
DDR", with no fault. What fits that signature:

1. **LLCC slice + `GBIF_SCACHE_CNTL1` SCID** — the only remaining candidate
   that changes *where* transactions route. Writes with no/unprogrammed SCID
   may be silently dropped by silicon-specific behaviour. Needs the
   early-attribute ordering fix (set before `iommu_attach_device()`), slice
   activation, and SCID programming. Rail-sensitive per CAF's own comment.
2. A write-path cache that never drains to DDR (same fix area as #1).
3. `CP_ME_INIT` mask / GMU AO CGC — weaker fits (CP and GMU demonstrably work).

Eliminated on this image: dispatch failure (RB=1), empty stream (dense valid
PM4), wrong geometry (rects correct), active zeroing (0xAA intact).