# FINDING: the SQE executes the draws — the defect is in the render backend

Image 262. Date: 2026-11-16. Follows `FINDING-COMMAND-GRAPH-VERIFIED.md`.

## The measurement

`ibtrace` is now change-driven: it polls `pc`/`pkt` at `udelay(1)` and logs
only on a transition. A whole submission completes in under ~100 us, so the
previous 100 us sampler structurally could not see the middle of the walk —
an instrument with a sampling rate below the phenomenon.

67 iterations, **3 transitions**, the entire visible execution:

```
step=000  pc=0x00000112  pkt=0x70460001   fence=0
step=001  pc=0x00000d00  pkt=0x70388003   fence=0    <-- CP_DRAW
step=002  pc=0x0000012e  pkt=0x70460004   fence=1
```

`TRC[seq=1] walk_end iterations=67 transitions=3`

## What this proves

**`pkt=0x70388003` is the `CP_DRAW` packet.** Identified independently and
three ways: 11 draws produced 11 of them; 60 draws produced 60; and one draw
produced 1 (`FINDING-COMMAND-GRAPH-VERIFIED.md`).

So the SQE:

1. starts at `pc=0x112`,
2. descends into the leaf buffer and enters a **draw-processing routine at
   `pc=0xd00` holding a live `CP_DRAW` packet**,
3. returns to its idle loop at `pc=0x12e`,
4. retires the submission (`fence` 0 -> 1).

Every previously-unknown link is now closed:

| Question | Answer | Evidence |
|---|---|---|
| Is the nested graph walked? | **yes** | PC enters the leaf at `0xd00` |
| Do the draws execute? | **yes** | `pkt=0x70388003` is `CP_DRAW` |
| Is the ring's work completing? | **yes** | `fence` lands, seqno-correct |
| Any faults? | **none** | `int0=0`, `cpint=0` throughout |
| Does the render target get written? | **NO** | all-zero, every path, always |

The `pc=0xd00` value is far outside the `0x112`/`0x12e` idle region, which is
consistent with a separate microcode routine entered specifically to process a
draw.

## So the defect is now pinned to the render backend

The GPU is handed a correct nested command graph. It walks it, dispatches a
`CP_DRAW` to the render engine, and retires cleanly without a single fault.
The render engine produces **no memory write whatsoever**.

Both the clear and the draws are affected:
- in **GMEM** mode the clear goes to an on-chip carveout and the resolve/store
  to DDR never happens;
- in **sysmem** mode the clear is an `RB_BLIT` that must reach DDR and does not.

A common cause for both, given the draws themselves are dispatched, is that
the **render target is never armed** — i.e. the GRAS/RB framebuffer and
attachment descriptors do not become live — so the RB has nothing to write to,
or rejects the blit.

There is also a specific a6xx mechanism worth testing first, from the freedreno
documentation:

> "At draw time, if the draw call is going to actually execute (some primitive
> is visible in the current tile), the SQE goes through the GROUP_IDs and for
> any with an update since the last time they were executed, it executes the
> corresponding fragment."

So a draw is **culled** when no primitive is visible in the current tile. If
the tile/bin geometry or scissor is wrong, every primitive could be culled and
every draw would be skipped without any fault — indistinguishable, from the
outside, from "the draw ran and drew nothing".

## Next step

Instrument the render backend during that `pc=0xd00` window — the walk tracer
now gives us a precise trigger for it:

1. Sample `RBBM_PERFCTR_GPU_BUSY`, the CCU status, and the GRAS/RB
   framebuffer/size registers on each transition, so we can tell
   **"RB never armed"** from **"RB armed and wrote elsewhere"**.
2. Check the tile/bin geometry and scissor actually programmed for a 64x64
   target, against what freedreno intends.
3. Only then consider whether the SQE firmware version gate
   (`a630_sqe.fw` fails upstream's check) causes draws to be culled.

Do **not** build the RB-write oracle yet. The draws are now proven to execute,
so a hand-built blit would run against the same unknown backend and a negative
result would still be ambiguous. Instrument first.

## Method note

The decisive change was not a new register — it was raising the sampling rate
above the duration of the phenomenon and logging transitions instead of
samples. Two earlier "the SQE never runs" conclusions were both artifacts of
sampling at 100 us and reading the result 50 ms late.