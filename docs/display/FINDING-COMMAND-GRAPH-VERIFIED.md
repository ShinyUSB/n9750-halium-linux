# The command graph is fully verified, end to end — ring → envelope → leaf draws

Date: 2026-11-16, image 261. Follows `FINDING-SUBMIT-PATH-PROVEN-SOUND.md`.

## The structure, verified

The a6xx command stream is a **nested indirect-buffer graph**, and the driver's
own comment at `a6xx_gpu.c:1109-1133` documents it:

> "the captured stream contains THREE nested CP_INDIRECT_BUFFER references…
> These are where the actual render commands live: the clear (tile_setup) and
> the draw/prologue buffers are all IB targets. So the one buffer we did dump
> was only ever the envelope."

Measured for one submit:

```
ring[163..166]  CP_INDIRECT_BUFFER -> 0x70002b000, size 0x15a   (opcode 63)
bo[10] @0x70002b000  dword 305 -> 0x70002d000 = bo[7]
bo[7]  @0x70002d000  the real draw stream
```

and `bo[7]` scales exactly with the workload:

| NDRAW | bo[7] nonzero | `CP_DRAW` (0x70388003) |
|---|---|---|
| 1 | 63 | **1** |
| 11 | 103 | **11** |
| 60 | 299 | **60** |

Every level of the graph is present, correctly addressed, and its size scales
correctly. The draws are genuinely present in the submission.

## Correction: my "IB is short by 260 dwords" was WRONG

`ibsize.py` compared the IB field against the stream in **bytes** and reported a
4x shortfall. The field is in **dwords**:

```
msm_gem_submit.c:522   submit->cmd[i].size = submit_cmd.size / 4;
msm_rd.c:387           uint32_t szd = submit->cmd[i].size; /* in dwords */
a6xx_gpu.c:1254        OUT_RING(ring, submit->cmd[i].size);
```

So `0x15a` = 346 dwords = 1384 bytes = exactly the envelope's length. **The IB
size is correct.** That was error #10 in this project — an instrument that
assumed a unit instead of reading it out of the source. Caught by grepping the
field definition before acting on it.

Recorded here rather than deleted, because the *reasoning* it produced was
sound and only the unit was wrong: if the IB size had genuinely been short, it
would have explained everything, and that is exactly why it was worth checking.

## What is now established

| Layer | Verdict |
|---|---|
| Ring (15 dwords, IB pointer, scratch, cache-flush-ts) | correct |
| IB size (`0x15a` dwords) | correct |
| Envelope `bo[10]` (constant, 237 dwords) | correct, carries nested IB refs |
| Leaf `bo[7]` (draws) | correct, scales 1 → 11 → 60 `CP_DRAW` |
| Relocations at every level | all resolve inside submitted BOs |
| Stream retires, fence lands, no faults | proven by the `ibtrace` burst |
| Render target written | **NEVER** |

The complete command graph is correct and submitted. The GPU retires it
cleanly. Nothing is written.

## The one thing never observed

Every piece of hardware evidence we have is from the **ring level**:

- `pkt=0x70bf8003` is opcode 63 = `CP_INDIRECT_BUFFER` — the SQE caught
  processing the ring's IB packet.
- `CP_SCRATCH_REG[2]` and `CACHE_FLUSH_TS` are emitted by `a6xx_submit()`
  into the ring.

We have **never once observed the SQE inside the envelope, and never inside the
leaf**. The fence landing proves the ring completed; it does **not** prove the
nested buffers were walked. Those are the levels where the render commands
actually live.

That is the gap, and it is narrow.

## Next step: trace the SQE's walk

Change the burst from a fixed 64 samples at ~100 us to a **change-driven trace
at ~1-2 us**, logging every transition of `(pc, pkt)` rather than every sample.
The whole submission completes in under ~100 us, which is why a 100 us sampler
sees only the start and the idle state.

Expected outcomes, stated in advance:

- **PC descends through several distinct values** ending in the leaf buffers →
  the graph is walked, and the defect is inside the render commands themselves
  (state never becomes live). Move to GRAS/RB instrumentation.
- **PC never leaves the ring-level IB packet** → the CP is not descending into
  the nested buffers at all. That is a very specific, very fixable finding, and
  it would explain every symptom with one mechanism.

This needs one small kernel change (`usleep_range` → `udelay`) plus a
change-driven logging mode, then a rebuild. It is worth it: it is the only
remaining measurement that distinguishes "the commands were not run" from
"the commands ran and drew nothing".