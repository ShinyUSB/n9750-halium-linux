# 🎉🎉 Image 195 — THE GPU IS ALIVE. First time in the project.

Kernel `#139`. One missing memory barrier.

## The fix

`drivers/gpu/drm/msm/adreno/a6xx_hfi.c`, `a6xx_hfi_queue_write()`:

```c
header->write_index = index;
spin_unlock(&queue->lock);
wmb();                                          /* <-- ADDED */
gmu_write(gmu, REG_A6XX_GMU_HOST2GMU_INTR_SET, 0x01);
```

CAF KGSL has this barrier at `kgsl_hfi.c:170-182`; our port had dropped it.
CAF's own comment states the invariant:

> "Memory barrier to make sure packet and write index are written before an
> interrupt is raised"

Without it, the GMU was kicked before the queue stores reached memory. It read
`rd == wr == 0`, found nothing to do, and went back to sleep — which is exactly
the "firmware ignores HFI" signature we chased for images 191–194.

## Before / after

| | images 191–194 | **195** |
|---|---|---|
| HFI fw_version | 5 s timeout, `ret=-110` | **`HFI-ACK id=1 error=0`, `ret=0`** |
| GMU consuming queue | `q0 rd=0` (idle) | **`q0 rd=221 wr=221`** |
| `hw_init suppressed` | yes | **0 occurrences** |
| `CP_RB_BASE` | `0x0` | **`0x1000`** |
| `CP_RB_CNTL` | `0x0` | **`0x20c`** |
| `CP_SQE_INSTR_BASE` | `0x0` | **`0x9000`** |
| CPF verdict | `CP is NOT CLOCKING (stalled, problem is the ring)` | **`CP is CLOCKING (ring is fine)`** |
| SMMU faults | 0 (since 192) | **0** |
| `GEM_SUBMIT` | `= 0` accepted | **`= 0` accepted** |

The `RINGHW` values (`RB_BASE=0x1000`, `RB_CNTL=0x20c`, `SQE_BASE=0x9000`,
`RB_RPTR==RB_WPTR==0x63`) now match the archived reference from the last boot
that ever programmed a ring, byte for byte.

`RB_RPTR == RB_WPTR` is the healthy steady state: the CP drained the ring and
is waiting for more work.

## How it was found

Consultation round 1 (Muse Spark 1.3 + Ling 3.1 Flash + Nemotron 3 Ultra).
Nemotron hypothesised the HFI doorbell was never rung. Checking that against
the source **refuted** it (`a6xx_hfi.c:190` does ring it) — but the check
exposed the missing `wmb()` two lines above.

Three of Nemotron's four hypotheses were wrong. The fourth led to the bug.
Exactly as `using-other-models.txt` warns: nothing a consultant asserts is a
fact until checked against the repo.

Muse Spark and Ling independently converged on the sptprac timeout being
load-bearing. **That turned out not to be the cause** — sptprac still times out
on image 195 and the GPU works perfectly. Their shared conclusion was wrong.

## An evidence artefact worth recording

Our log has printed `doorbell=0x01` on every boot since the HFI instrumentation
was added, and I repeatedly cited it as proof the kick worked. It is a
**hardcoded literal in the printf format string**:

```c
pr_info("HFI-B fw_version post-write q0 rd=%u wr=%u doorbell=0x01 data=", ...);
```

It reports a constant, not a register. It was never evidence. A diagnostic that
cannot fail is not a diagnostic.

## Where the project actually stands

Proven working, in one live run:
- GMU firmware boots and self-initialises
- HFI transport works end to end (3 messages ACKed, `error=0`)
- GMU actively drains the command queue
- Command ring is programmed in hardware
- **CP is clocking and executing**
- `GL_RENDERER = FD640`, `GEM_SUBMIT = 0` accepted

**Still not demonstrated: visible pixels.** The remaining gap is that the CP has
consumed what it was given but we have not yet read back rendered output, and
no interrupt has been observed firing (`/proc/interrupts` shows no `kgsl_hfi_irq`
or `A640` activity on this pass, though HFI clearly completed). Next question is
whether commands execute to completion and whether fences/interrupts retire.

Do not revert images 192, 193 or 195. Each is load-bearing and verified.