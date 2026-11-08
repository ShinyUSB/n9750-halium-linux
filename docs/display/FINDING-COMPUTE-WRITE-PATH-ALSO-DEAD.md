# FINDING: compute → SSBO write path is ALSO dead — the defect is not RB-specific

Date: 2026-11-16
Baseline: image 263 (unchanged — no kernel edit, no flash, no risk taken)

## Why this test was re-run

`consult/PLAN.md` §1 flagged that the original compute result (IMAGE-245 §3 T1)
predated the Oct-15/16 submit-path fixes and should be re-run once on image 263
before being load-bearing. It is a 10-minute userspace test with zero device
risk, and it is a genuine discriminator between "RB is broken" and "the shared
shader-domain write path is broken".

## 0. The existing harness could not answer the question

`wrtest.py` (on device, md5 `73d67b4a2…`, matches the local copy) allocates both
target buffers with `glBufferData(..., None, ...)` — **no prefill** — and treats
"nonzero bytes == 0" as "shader->buffer write dead".

That verdict is unfalsifiable. An untouched buffer, a failed upload, and a GPU
that wrote zeros are all byte-identical from that measurement. The harness
cannot distinguish "the GPU wrote nothing" from "the GPU was never asked".

Rewritten as `harnesses/wrt2.py` with a **phase-0 oracle**: the buffer is
prefilled from the CPU with a known byte pattern, read back through the *same*
GL map path the verdict uses, and the verdict is refused if the roundtrip fails.

## 1. Results (all oracle-validated)

| Test | Path | Prefill intact | Expected pattern | Verdict |
|---|---|---|---|---|
| T1 | compute shader → SSBO | **256/256 = 0xAA** | 0/64 | **GPU wrote nothing** |
| T2 | vertex → transform feedback buffer | **1024/1024 = 0x5A** | 0/64 records | **GPU wrote nothing** |
| T3 | fragment → FBO (control) | n/a (GPU-mediated read) | — | after-clear `[0,0,0,0]`, after-draw `[0,0,0,0]` |
| T4 | `GL_TIME_ELAPSED` | — | — | **MISSING — harness bug, not a negative** |

T4 aborted on a `ctypes` signature error (`glGetQueryObjectuiv` declared with
`c_uint` for the GLenum, so the call was rejected as arity-mismatched). The
timer datapoint is therefore **absent**, not negative, and is recorded here as
unmeasured rather than silently dropped.

## 2. The dispatch really is submitted — proven, not assumed

`wptr_dw` (the `CPDUMP` software ring cursor, `a6xx_gpu.c:3117`) did **not**
advance across T1 — `294 -> 294` with one dispatch, and `373 -> 373` with 64.
Taken alone that would make the T1 negative ambiguous: it would be consistent
with Mesa never flushing the dispatch at all.

That was resolved directly, by comparing the actual command stream with and
without dispatches under `ibdump`:

| Run | dword-lines captured | submits | `CP_LOAD_STATE6_COMPUTE` (`0x70bf8003`) |
|---|---|---|---|
| `NDISP=0` | 88,319 | 4 | 22 |
| `NDISP=256` | **2,082,252** | 8 | **769** |

A **23.6×** increase in submitted command stream, and roughly three
`CP_LOAD_STATE6_COMPUTE` packets per dispatch. The compute work is built,
submitted, and programs the compute state.

Supporting evidence in the same runs:
- `HANGCHK141: enter fence=23 stored=18 submitted=23` — submits retire normally
- `CP_TIMESTAMP … clocking 1` — the CP keeps fetching
- no faults: the only matches for fault/hang/oops/reset in the whole capture are
  the informational `HANGCHK141` lines (their own tag contains "HANG")
- GL reported no error after dispatch or after `glMemoryBarrier`

## 3. What this changes

The framing shifts. The live statement is no longer

> the render backend produces no DDR write

but

> **no shader-domain write path produces a DDR write — neither RB render stores
> nor SP compute stores nor vertex transform-feedback stores — while the CP
> domain demonstrably writes DDR (`memptrs->fence` increments) and raises no
> fault.**

So the defect is **common to SP and RB**, which rules out the entire family of
RB-specific explanations that the corpus has been circling: GMEM binning, tile
visibility, `RB_CCU_CNTL`, `RB_DBG_ECO_CNTL`, blend/depth state, the RB render
descriptor, `RB_CCU` carveout sizing, and anything else local to the render
backend.

Combined with `FINDING-MESA-EXONERATED-RT-AND-VIEWPORT-CORRECT.md` (every Mesa
render-target, viewport and blit field verified correct against the real XML),
and with the three kernel consults reaching clean negatives, the search space
that survives is narrow and *shared*:

- GPU→DDR transaction formation / GBIF path
- SMMU or translation behaviour **on the write direction only** (fence writes
  prove translation works, but the fence is written by the CP's own store, which
  may take a different path than a shader-domain store)
- UCHE / write-back cache flush or invalidation — every shader-domain store
  would be lost here while the CP's bypasses it
- a secure-mode / privilege gate on the shader write path
- SQE microcode not driving the shader-domain store correctly (the stale
  `a630_sqe.fw` that fails upstream's own `a6xx_ucode_check_version()` gate)

## 4. What this removes

X1 (aperture-correct cluster readback) was recommended partly to answer "did the
state become live in hardware". That question is now **demoted further**: even
if Mesa's state is perfectly live in GRAS/PS, it demonstrably does not produce a
store. The aperture route is still worth doing — but as a way to *read* the
shared-path state (UCHE ranges, flush/invalidate config, secure/privilege bits),
not as a way to test whether render state landed.

## 5. Standing caveats

- T3's readback is GPU-mediated (no CPU oracle exists for an FBO), so T3 is a
  control, not an independently validated measurement.
- T4 is unmeasured; a fixed T4 would add the "does the GPU report doing work"
  datapoint, which is still worth having.
- `wptr_dw` not advancing is unexplained. It is not evidence of anything about
  submission — that was settled by the stream-volume comparison above — but it
  should not be used as a submit indicator in future harnesses.

## Files

- `harnesses/wrt2.py` — oracle-validated compute/TF/FBO probe (`NDISP` env var)
- supersedes `/usr/local/bin/wrtest.py` for this purpose; the original is left
  untouched for provenance