# FINDING: the submit path is PROVEN sound end-to-end — the fence timestamp lands

Image 261, `ibtrace` burst sampler. Date: 2026-11-16.
Supersedes `STATE-STREAM-NEVER-RETIRES.md`.

## The measurement

64 samples ~100 us apart, taken immediately after `a6xx_flush()`, covering the
entire execution of the submission. Full workload (clear + 11 draws) was a
**single submit, seq=1**.

```
TRC[seq=1 i=000] pc=0x00000112 pkt=0x70460001 rptr=0x00072 wptr=0x00072
                scratch=0x00000000 int0=0x00000000 cpint=0x00000000
                fence=0 rptraddr=0x700000000 flush_ts=0
TRC[seq=1 i=001] pc=0x0000012e pkt=0x70460004 rptr=0x00072 wptr=0x00072
                scratch=0x00000001 int0=0x00000000 cpint=0x00000000
                fence=1 rptraddr=0x700000000 flush_ts=0
   ... i=002..063 identical to i=001
```

## What this proves

**1. The submission retires. Fully.**

Between sample 000 and 001:

- `CP_SCRATCH_REG[2]`: `0x0 -> 0x1` — the driver's preamble scratch write
  landed, with the value being the submit seqno.
- `memptrs->fence`: `0 -> 1` — **the `CACHE_FLUSH_TS` timestamp write landed
  in memory, carrying seqno 1.**

That fence write is the last packet of the stream. If it landed, the whole
command stream preceding it executed. `RPTR == WPTR == 0x72` from the first
sample, i.e. the entire 15-dword ring was consumed.

**2. The SQE runs, then goes idle.**

`pc` moved `0x112 -> 0x12e` across the boundary and then parked at `0x12e`
with `pkt=0x70460004` for the remaining 63 samples (~6.4 ms). That is exactly
the "healthy idle SQE" shape I predicted when retracting the "frozen SQE"
claim: it advances while there is work, then settles at a fixed spin address.
A post-hoc sample could never have distinguished those two states — which is
precisely why the burst was needed.

**3. `CP_RB_RPTR_ADDR` IS programmed.**

`rptraddr=0x700000000`, not `0x0`. The old tree note claiming
`RB_RPTR_ADDR=0x0` is another stale comment. This also closes the loose end
from the compile error — the field I added speculatively turns out to be fine.

**4. No faults, at any sample.**

`int0=0x00000000` and `cpint=0x00000000` across all 64 samples: no AHB error,
no hang detect, no opcode/ucode/hw_fault/protect/vsd/illegal error.

## My "stream never retires" conclusion was WRONG

`STATE-STREAM-NEVER-RETIRES.md` rested on `cache_flush_ts=0` read 50 ms after
the fact. The burst shows `flush_ts=0` at **every** sample, yet the fence
clearly landed. So that bit is transient — almost certainly read-and-cleared by
the driver's IRQ handler inside the ~100 us window between the timestamp write
and our second sample.

I pre-registered exactly this falsification in `PLAN-IMAGE-261-TIMED-SAMPLER.md`:

> "If `flush_ts` fires, then the earlier `cache_flush_ts=0` was itself a
> sampling artifact and every conclusion ... must be re-examined."

It fired. The conclusion was wrong and is retracted.

**The lesson is the same one that has now recurred eight times, and it is
worth stating plainly: a single register read taken after the fact cannot
distinguish "did not happen" from "happened and was cleared".** Every prior
verdict in this project — scratch register, fence, RPTR, `sqe_pc`,
`cache_flush_ts` — was exactly that. The burst replaced them with a time series
and immediately overturned the conclusion.

## Instrument bug found and fixed (mine)

My first attempt produced **zero** TRC lines, because `bringup261.sh` set
`ibtrace=64`, printed it, then set it back to `0` before running the probe:

```sh
echo "ibtrace=64" > .../ibtrace
echo "ibtrace=0"  > .../ibtrace      # <-- probe ran with this
```

Same failure class as every other dead instrument: one that cannot produce
data, and would have been easy to misread as "the sampler found nothing".
Fixed by setting `ibtrace` before the probe; verified `ibtrace=64` on the wire.

## Where the bug is now

| Layer | Verdict | Evidence |
|---|---|---|
| Command stream contents | **correct** | byte-level diff, `FINDING-SUBMISSION-IS-COMPLETE.md` |
| Ring preamble + IB pointer | **correct** | 15 dwords verified, IB -> valid cmdstream IOVA |
| CP executes the stream | **PROVEN** | `scratch 0->1`, `fence 0->1`, seqno correct |
| SQE runs then idles | **PROVEN** | `pc 0x112 -> 0x12e`, then stable |
| RPTR shadow programmed | **PROVEN** | `rptraddr=0x700000000` |
| Faults | **none** | `int0=0`, `cpint=0` at every sample |
| Render target written | **NO** | RT BO stays all-zero; clear colour in no buffer |

The entire submit path — userspace, driver, ring, CP, SQE, fence — works.
The failure is strictly downstream: the work is submitted, executed and
retired, and **the render produces no DDR write at all**.

## Next step

With retirement proven, the RB-write oracle is finally meaningful again: a
stream that retires can be inspected for whether the RB acted on it. The
sharpest question is now narrow and testable — the GPU completes the
submission but no buffer changes, so either

1. the RB/GRAS never receives or accepts the blit/clear, or
2. it writes somewhere other than the buffer the command stream addresses.

Re-run the sysmem render-path case under `ibtrace` and compare the RT's
contents **before and after** within the same submit, and check whether
`GRAS_2D_*` / `RB_*` state in the stream ever becomes live. Keep the burst on:
it is the only instrument in this project that has never once produced a false
reading.