# STATE (end of session 2026-11-16): submit path proven, render provably never writes

Image 261 + on-device analysis. Supersedes `STATE-STREAM-NEVER-RETIRES.md`
and `FINDING-CP-CONSUMES-RING-RB-WRITES-NOTHING.md`.

## The proof, in one table

Every layer between the GL call and DDR has now been measured directly:

| Layer | Verdict | Direct evidence |
|---|---|---|
| GLSL compiles + links | works | `prog linked=1 vs=1 fs=1` |
| Command stream built | **correct** | state prologue, 11 `CP_DRAW`, 13/13 relocations resolve |
| Ring + IB pointer | **correct** | 15 dwords, IB -> valid cmdstream IOVA, `rptr==wptr` |
| CP executes the stream | **PROVEN** | `scratch 0->1->2`, `fence 0->1->2`, seqno-correct |
| SQE active mid-flight | **PROVEN** | `pc=0x12a pkt=0x70bf8003` (the IB packet) caught in the act |
| Stream retires | **PROVEN** | `CACHE_FLUSH_TS` timestamp lands, `fence` tracks seqno |
| Faults | **none** | `int0=0`, `cpint=0` at every one of 64 samples |
| RPTR shadow programmed | **PROVEN** | `rptraddr=0x700000000` |
| Render target written | **NEVER** | `VERDICT: RENDER TARGET IS ALL ZERO` (0/4096) |

The submit path is not merely "probably fine". It is measured working at every
step, in both the GMEM and sysmem render paths, with the SQE caught mid-fetch.

## And the render writes nothing

`sysmem` is the configuration where the clear *must* reach DDR — there is no
GMEM to absorb it. Result:

```
=== RENDER TARGET 0x102186000 ===
  nonzero dwords: 0 / 4096 dumped
  VERDICT: RENDER TARGET IS ALL ZERO
```

Full render-path matrix, all with complete captures, all `FD640`:

| `FD_MESA_DEBUG` | stream retires | RT written |
|---|---|---|
| default (GMEM) | yes | **no** |
| `sysmem` | yes | **no** |
| `noubwc` | yes | **no** |
| `nobin` | yes | **no** |
| `notile` | yes | **no** |
| `noubwc,sysmem` | yes | **no** |

So: the GPU is handed a correct, complete, correctly-addressed instruction
stream; it executes it, retires it, reports no error — and produces **no
memory write of any kind**. That is now established from both directions.

## The clear colour is a register write, not a pixel

Worth stating because it looked like a breakthrough twice:

```
0x40a98901 0x00000100   0x48886501 0xffff0000   0x00000000
                    ^^^^^^^^^^^^^^^^^^^^^^^^^
```

`0x48886501` is a `CP_REG_WRITE` header. `0xffff0000` is the clear colour
being **written into a register inside the command stream** — it describes
the clear. It appears in the 32768-byte state buffer at dword 2681, in every
configuration, and is **not** evidence that any pixel was written. My
`grep -c` counter for it was a misleading instrument and I have replaced it
with a parser that distinguishes the two cases.

## Corrections made this session (all self-inflicted, all caught)

1. "The CP consumed the ring" — RPTR is the ROQ prefetcher, a different clock
   domain. Not proof of execution.
2. "The SQE is frozen at `0x12e`" — sampled after `msleep(50)`; that is what a
   *healthy idle* SQE looks like.
3. "The stream never retires" — `flush_ts` is transient and cleared by the IRQ
   handler; `fence` landing proves retirement.
4. "`memptrs_iova == 0`" — stale comment; it is `0x700000000`.
5. "`RBBM_CLOCK_CNTL=0x8aa8aa02` is wrong" — that is the A630 value; A640's
   `0x8aa8aa82` is correct and is what we read.
6. "Missing `CP_SQE_MEMBASE`" — upstream never writes it either.
7. "`0xffff0000` found — breakthrough" — a register write in the stream.
8. `bringup261.sh` reset `ibtrace=0` before the probe, so the first run
   produced zero samples.
9. The capture file was lost twice: once to the hard reset (ring buffer
   discarded), once to truncation. Fixed by `dmesg -W` to a file plus on-device
   parsing.

**Nine instruments that could not distinguish the states they were meant to.**
Each one produced a confident, wrong answer. The only instruments that have
never produced a false reading are the ones that (a) run continuously,
(b) record to durable storage, and (c) parse rather than pattern-match.

## The one remaining unknown

Everything up to and including execution is proven. The failure is strictly
inside the render engine: the stream is executed, but no store reaches DDR.

`a6xx_gpu.c:644` names the fork directly:

> "RB never, CP/SP -> backend never engaged (dispatch fork; CP_ME_INIT /
> GMU AO CGC are next)"

We have now independently confirmed "CP/SP yes". The RB half has never been
observed doing anything. Since `CP_ME_INIT` is emitted with the correct
upstream mask (`0x2f`, called at line 2398) and `CP_RB_RPTR_ADDR` is
programmed, the untested branch is the **GMU AO clock-gating** path and, more
importantly, the fact that we never emit `CP_SMMU_TABLE_UPDATE`
(`CP_SMMU_TABLE_UPDATE = 83` is in our PM4 header, unused) — the opcode the
Linaro SMMU talk describes as how the GPU is told its page table.

## Next steps, ranked

1. **Instrument the RB.** Extend the `ibtrace` burst to sample
   `RBBM_PERFCTR_GPU_BUSY`, the CCU and `GRAS_2D_*` state so we can tell
   "RB never armed" from "RB armed and wrote elsewhere". This is the one
   unobserved block and the smallest remaining unknown.
2. **Diff our `hw_init` against upstream, in order.** Two differences already
   found: we start the SQE (`HWINIT[09]`) *before* `a6xx_set_hwcg`
   (`[10]`) and interconnect start (`[11]`), where upstream does clock gating
   first and starts the SQE last; and upstream's `a6xx_set_hwcg` has a whole
   branch (`CGC_GLOBAL_LOAD_CMD` / `CGC_P2S_TRIG_CMD`) that we lack entirely.
3. **Decide on `CP_SMMU_TABLE_UPDATE`.** Note the counter-evidence: the fence
   write at `0x700000004` *does* land, so the SMMU works for that address.
   That weakens a naive "no page table" story and must be resolved rather than
   assumed.

Do **not** build the RB-write oracle yet. A hand-built blit would also be
executed by an RB whose failure mode is unknown, so a negative result would
still be ambiguous. Instrument the RB first.