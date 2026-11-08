# BO-content test — nothing the GPU is asked to write ever lands in memory

Date: 2026-10-11
Image: 220, no rebuild. Used the existing `BOS[]` instrumentation.

## Test

After a validated CPU→GPU→CPU round trip (FBO COMPLETE), capture every submit's
BO contents and look for the known values — the uploaded pattern `(200,100,50,255)`
and the clear colour `(0,255,0,255)`.

## Result: last submit (seq=52), every BO dumped

```
bo[0] iova=0x20000 flags=0x6003 nonzero=1     first8=[0x14, 0, 0, 0, 0, 0, 0, 0]
bo[2] iova=0x34000 flags=0x6003 nonzero=0     first8=[0, 0, 0, 0, 0, 0, 0, 0]
bo[3] iova=0x31000 flags=0x6007 nonzero=202   first8=[0x70460004, 0x1d, 0x20000, ...]
bo[4] iova=0x16000 flags=0x6003 nonzero=1     first8=[0x4, 0, 0, 0, 0, 0, 0, 0]
bo[5] iova=0x2f000 flags=0x6007 nonzero=145   first8=[0x70460001, 0x19, 0x70460001, ...]
```

**A clean and very informative split:**

- `flags=0x6007` BOs (`bo[3]`, `bo[5]`) — the **command streams** — are full of
  real PM4 packets (`0x70460004`, `0x70460001`, …). These are alive and correct.
- `flags=0x6003` BOs (`bo[0]`, `bo[2]`, `bo[4]`) — the **data buffers** — are
  essentially empty. `bo[2]` has **zero** non-zero dwords across 1024 dumped.

**The clear colour and the uploaded pattern appear in NO buffer.**

Resource allocation for the round trip (from `FD_MESA_DEBUG=msgs`) confirms which
is which:

```
target=2d, r8g8b8a8_unorm, 64x64x1, usage=0, bind=a      <- the FBO render target (UBWC)
target=2d, r8g8b8a8_unorm, 64x64x1, usage=4, bind=20000a <- upload staging (linear)
target=2d, r8g8b8a8_unorm, 1x1x1,   usage=4, bind=20000a <- readback staging (linear)
handle_rgba_blit: linear -> UBWC    (upload)
handle_rgba_blit: UBWC  -> linear   (readback, twice)
```

A 64x64 RGBA8 buffer is 16 KiB = 4096 dwords; our dump covers the first 1024.
**All 1024 of the render target's dwords are zero.** Whether it is UBWC- or
TILED-encoded, written content would not be all-zero: compressed or tiled green
data is still non-zero.

## Conclusion: world (a), not world (b)

The two candidate explanations after the matrix were:

- (a) No GPU write ever lands.
- (b) Writes land but readback reads different memory.

**This test selects (a).** The render target buffer itself, inspected directly in
GPU memory without going through freedreno's blitter, is empty. Nothing was ever
written to it — not the CPU-uploaded pattern, not the clear colour.

So this is *not* a readback-addressing bug. The GPU genuinely does not perform
the writes.

Corroborating: the command streams in the same submit are fully populated and
correct, so the CPU side built valid commands and they reached memory. The
failure is strictly GPU-side execution.

## What this narrows to

Everything now points at the GPU not executing the command stream it is handed,
despite:
- bring-up healthy (`BABEFACE`, `hw_init ret=0`),
- CP_SCRATCH and the ring fence advancing 1,2,3… (the CP sequencer runs),
- command-stream BOs correctly populated in GPU memory,
- two architecturally different write paths (2D `CP_BLIT` and shader-based
  `util_blitter` render pass) both failing,
- UBWC on and off both failing,
- sysmem and gmem both failing.

Remaining candidates, in rough order:

1. **The CP does not fetch/execute nested indirect buffers.** We have never
   observed a single *write* command take effect. If `CP_INDIRECT_BUFFER` is not
   being followed, every actual render/blit command — which all live in nested
   IBs — is silently skipped, while the ring-level fence/scratch still advances.
   This fits every observation.
2. **Page-table / SMMU mapping is absent for data BOs.** The command streams
   are readable (we read them from the CPU, which proves nothing about GPU
   visibility); data writes may fault silently.
3. **The SQE/microcode is not fetching.** Earlier evidence had `sqe_pc=0x12e`
   with `CP_SQE_CNTL=1`, but we have never proven a *nested* IB fetch occurs.

## The decisive next test

Distinguish (1) from (2)/(3): find out whether the GPU fetches a nested IB at
all. Cheapest available signal — compare a submit whose commands are **inline in
the ring/cmd[0]** against the current shape where all real work sits behind
`CP_INDIRECT_BUFFER`. If inline commands also fail to write, fetching is not the
issue and it is (2). If inline commands DO write, then nested IB fetch is broken
and we know exactly where to look.

Concretely: `fd6_emit_ib` currently emits a nested IB for the clear. A debug
build (or a small Mesa patch) that inlines the same `tile_setup`/`draw` ring
content directly into `batch->gmem` would settle it in one run.

## Caveat

We cannot currently see a clear in any stream (it has never been captured), and
the `BOS[]` dump caps at 1024 dwords, so we inspect only the first quarter of
the 64x64 render target. A write confined to the last 3/4 would be missed —
though a full-surface `glClear` writes all of it, so that is unlikely.