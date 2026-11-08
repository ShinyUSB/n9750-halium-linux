# STATE: end of session 2026-11-16 — submission proven, CP writes proven, RB writes unproven

Device restored to image 263 (`b73519bf…`), the known-good walk-tracer build.

## What is established, and how solidly

| Layer | Verdict | How it was proven |
|---|---|---|
| GLSL compiles/links | works | `prog linked=1 vs=1 fs=1` |
| Command stream built | **correct** | byte-level diff: state prologue, N `CP_DRAW`, 13/13 relocations resolve inside submitted BOs |
| Nested command graph | **correct** | ring IB → envelope (`bo[10]`) → leaf (`bo[7]`); leaf holds exactly N `CP_DRAW` (1→1, 11→11, 60→60) |
| IB sizes | **correct** | `submit->cmd[i].size` is in **dwords** (`msm_gem_submit.c:522`); `0x15a` = 346 dwords = exact envelope length |
| Ring executes | **PROVEN** | `CP_SCRATCH_REG[2]` tracks seqno, `fence` lands, `rptr==wptr` |
| SQE walks the graph | **PROVEN** | change-driven trace caught `pc=0xd00 pkt=0x70388003` (**`CP_DRAW`**) and `pc=0xeb pkt=0x70348003` (**`CP_LOAD_STATE6_FRAG`**) |
| Stream retires | **PROVEN** | `CACHE_FLUSH_TS` timestamp lands, seqno-correct |
| Faults | **none** | `int0=0`, `cpint=0` at every sample of every run |
| **CP → DDR writes** | **WORK** | `memptrs->fence` increments 0→1→2: the GPU is writing timestamps to system memory |
| **RB → DDR writes** | **NEVER OBSERVED** | render target all-zero in every config, forever |
| GMEM / UBWC / tiling / binning | **ruled out** | full `FD_MESA_DEBUG` matrix, all fail identically |
| `memptrs_iova`, `RB_RB_RPTR_ADDR`, `RBBM_CLOCK_CNTL` | all fine | 0x700000000 / 0x700000000 / 0x8aa8aa82 (the A640 value; 0x8aa8aa02 is A630's) |

## The one asymmetry that matters

The GPU demonstrably writes to DDR — the CP writes fence timestamps into
`memptrs` every submit. What has **never** happened, in any configuration, is a
write from the **render backend**. That split is the entire remaining problem,
and it rests on measurements that cannot fail rather than on inference.

## Why reading the render backend kept failing

Four attempts, all abandoned:

| # | method | outcome |
|---|---|---|
| 263 | CPU MMIO readback of GRAS/RB registers | **unusable** — a6xx clusters are *context-banked*; the CPU reads a different context than the CP wrote. Also `PERFCTR_GPU_BUSY_MASKED` read a constant `0x002f7fff` while `ALWAYS_ON_COUNTER` advanced, i.e. the counter isn't counting — which is *not* evidence the engine is idle. |
| 264 | `CP_REG_TO_MEM` into a lazily allocated BO | **deadlock** — `msm_gem_kernel_new()` inside `a6xx_submit()` deadlocked against `struct_mutex`; kernel thread stuck forever holding it. |
| 265 | same, allocated at init inside `hw_init` | **deadlock** — `a6xx-load` in D state inside `msm_gem_new_impl`. Upstream escapes this by calling `a6xx_ucode_load()` from `msm_gpu_init()` *before* `hw_init`; this CAF tree calls it from inside `hw_init`. |
| 266 | `CP_REG_TO_MEM` into the memptrs tail (no new allocation) | **hard reset of the GPU.** The packets as emitted are wrong or unsupported. |

**Nothing was learned from 264/265/266.** The capture buffer was enlarged but
the `CP_REG_TO_MEM` emission itself has never successfully run.

## Two process failures worth recording

1. **I twice declared the device "frozen" from a command timeout and was wrong
   both times.** The device was alive; what blocked was `a6xx_gpu_cpstate`,
   which takes `struct_mutex` and therefore blocks whenever a submit holds it.
   The cheap decisive check is `ps -eo pid,stat,wchan,comm` looking for D-state
   — run that *before* concluding anything from a timeout.
2. **The capture file was written but empty** after the hard reset: `dmesg -W`
   was killed before flushing. File-based capture survives ring-buffer loss but
   not a reset, because the writing process dies with it.

## Instrumentation count

**Eleven** instruments in this project produced confident, wrong answers:
point-in-time register reads (`CP_SCRATCH_REG`, fence, `RPTR`, `sqe_pc`,
`cache_flush_ts`), the `DANGLING` scan (exact IOVA equality instead of
containment), the "clear colour in no BO" test (GMEM clears go on-chip),
"~2 dwords per draw" (wrong buffer), `memptrs_iova == 0` (stale comment),
`RBBM_CLOCK_CNTL` (A630 value), "missing `CP_SQE_MEMBASE`" (upstream doesn't
write it either), `ibtrace` reset to 0 before the probe, and the IB-size check
that assumed bytes when the field is dwords.

Only instruments that (a) run continuously, (b) record durably, and (c) parse
rather than pattern-match have been reliable — which is how the SQE walk was
finally caught.

## The remaining question, and the cost of answering it

**Can the RB write DDR at all?**

The current evidence cannot separate:
- *Mesa emits state that never becomes live* (userspace bug, fixable), from
- *the render backend is non-functional* (much deeper, possibly not fixable
  without new hardware work).

Distinguishing them requires driving the RB from something other than Mesa. Both
available routes carry real cost:

- **Hand-built PM4** — forbidden by standing project rule; guessing layouts has
  already produced two fabricated claims. It would now be grounded in
  `fd6_blitter.c:796` and the tree's headers, but it is ~40 dwords of encoding.
- **Replay of Mesa's own bytes** — `MSM_INFO_GET_IOVA` *is* available
  (`msm_drv.c:1715`), so real captured dwords could be patched and re-submitted
  into buffers we control. This avoids inventing encodings, but it is a real
  replay implementation: copy every captured BO, patch the RT IOVA, rewrite the
  nested IB pointers and sizes, resubmit. Several new failure modes.

Note the asymmetry in interpretation: a **positive** result (the replayed clear
writes) is decisive — Mesa's state is the bug. A **negative** result is
ambiguous, because dangling references in the replayed stream could also explain
it.

## Recommendation

1. **Keep 263 as the baseline.** It is stable, has the walk tracer, and carries
   none of the `ibcap` hard-reset trap.
2. **Decide explicitly whether the replay oracle is worth building.** It is the
   only remaining experiment that can split the remaining hypothesis space, but
   it is a substantial piece of new code on a project that has just spent three
   device-breaking changes on instrumentation.
3. If the oracle is declined, the honest fallback is to state the conclusion we
   can support: *the submission is correct, the SQE executes the draws, the CP
   writes to DDR, and the render backend produces no writes in any tested
   configuration* — and treat "why does the RB not write" as unresolved rather
   than manufacture another partial answer.