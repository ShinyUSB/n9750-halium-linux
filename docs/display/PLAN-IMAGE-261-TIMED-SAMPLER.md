# Image 261 experiment: what the time-resolved sampler can and cannot show

## The instrument

New module param `ibtrace` (samples per submit, 0 = off, default off):

```c
static unsigned int ibtrace;
module_param(ibtrace, uint, 0644);
```

Called from `a6xx_submit()` **immediately after `a6xx_flush()`** and before the
pre-existing `msleep(50)` block, so samples land while the submission is in
flight:

```c
a6xx_flush(gpu, ring);
a6xx_trace_burst(gpu, ring, submit->seqno);   /* NEW, image 261 */
rbobs_poll(gpu, ring, submit->seqno);
```

Each sample, ~100 us apart:

```
TRC[seq=N i=000] pc=0x........ pkt=0x........ rptr=0x..... wptr=0x.....
                 scratch=0x........ int0=0x........ cpint=0x........
                 fence=N rptraddr=0x........ flush_ts=0|1
```

- `pc`/`pkt` via the documented indexed protocol (write `CP_SQE_STAT_ADDR`,
  read `CP_SQE_STAT_DATA`); index 0 = SQE PC, index 1 = current packet.
- `scratch` = `CP_SCRATCH_REG(2)`, which flips to the seqno at the end of the
  driver's preamble, so it marks how far the CP got.
- `int0` bit 20 (`0x00100000`) = `CP_CACHE_FLUSH_TS`, the "fence landed" bit.
- Burst exits early once `RPTR==WPTR` **and** `flush_ts` is set, so a healthy
  submit costs one or two samples.

## Why this is the right instrument (and why the last nine were not)

Every previous verdict came from ONE sample taken after completion:

| earlier "proof" | why it could not fail | verdict |
|---|---|---|
| `CP_SCRATCH_REG[2]` advances | point sample, no order | CP *does* execute — **true, kept** |
| `RPTR` tracks `WPTR` | ROQ prefetcher, other clock domain | ring *prefetched* only |
| `sqe_pc` constant at `0x12e` | sampled after `msleep(50)` | **false** — idle loop, retracted |
| `DANGLING` IOVA lines | exact base-IOVA equality | **false** — real offsets |
| "no submit" (compute) | deferred submit, not absent | **false** — retracted |
| clear colour absent from BOs | GMEM clear goes on-chip by design | **false** — instrument |
| `~2 dwords/draw` | wrong buffer | **false** |
| `memptrs_iova == 0` | stale comment, fixed by image 234 | **false** |
| per-block `0xdeadbead` | absent-register sentinel | not a fault |

The burst replaces a point sample with a time series, which is the only thing
that can show *order* and *stall location*.

## Expected outcomes

| observation | meaning |
|---|---|
| `pc` advances across samples | SQE runs; look for where `flush_ts` fails |
| `pc` pinned across the whole burst | SQE genuinely stuck at that PC — the first hard evidence for that claim |
| `scratch` flips early, `flush_ts` never sets | CP reaches end of preamble, stalls at `CACHE_FLUSH_TS` |
| `rptr` advances, `scratch` never flips | CP not executing at all; ring only prefetched |
| `cpint` non-zero at any sample | a real fault, and `cpint` names the flavour |
| `flush_ts` sets, but RT still zero | CP retires cleanly → fault is downstream of the CP after all |

**Whatever happens, `flush_ts` not firing remains the load-bearing fact.** The
burst tells us *where* it stops, not *why*.

## Falsification conditions — stated up front

To avoid reading a desired conclusion out of the trace:

- If `pc` advances but `flush_ts` still never fires, the "SQE wedged" theory is
  dead for good and the next target is the `CACHE_FLUSH_TS` path (its
  timestamp address is `0x700000004`, which is mapped).
- If `pc` is pinned, I will disassemble the microcode at that PC before
  claiming anything. `qrisc`/`adreno-regs` are 404, so I will **not** guess
  the ISA; if I cannot disassemble, I report the PC as an observation only.
- If the burst shows `flush_ts` firing, then the earlier `cache_flush_ts=0` was
  itself a sampling artifact (read too late / bit already cleared) and every
  conclusion in `STATE-STREAM-NEVER-RETIRES.md` §"the stream never retires"
  must be re-examined.
- `rptraddr` is logged because `CP_RB_RPTR_ADDR` was observed `0x0`; if the
  RPTR shadow address is genuinely unset, the hardware RPTR mirror and the
  software `memptrs->rptr` can diverge, which would explain
  `rptr=519` vs `RPTR=534` seen earlier.

## Risk

Low. Read-only apart from the documented `CP_SQE_STAT_ADDR` index selector
(which the existing `SQE214` code already writes). Default `ibtrace=0`, so the
submit path is byte-for-byte unchanged unless the param is set. No build-time
change to behaviour; no register writes the driver does not already make.

The one real hazard is the known one: submitting GL work can hard-reset the box
with no kernel record. Run with `/root/poller.sh` up and `cpstate_poller.py`
recording, as before.