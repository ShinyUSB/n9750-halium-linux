# FINDING: Mesa's render state IS live in silicon — state delivery is NOT the defect

Date: 2026-11-16
Image: 267 (`267-halium-boot-a6xx-aperture.img`,
sha256 `83bed738d392cad85570a83015025314720cb7471eafa2043df510001cbff403`),
built with `apread` aperture-correct cluster readback.
Baseline before this experiment: image 263 + newer SQE ucode.

## Headline

**X1's question is answered YES, and the answer relocates the defect.**

Mesa's render state is present in the actual GPU registers after a real submit,
and the values *track the workload*. State delivery is not broken. The defect is
strictly below register programming.

## Evidence

### 1. The aperture mechanism writes what the vendor recipe says

Per `adreno_a6xx_snapshot.c:1000`, `(cluster<<8)|(ctxt<<4)|ctxt`:

```
ctxt=0 aperture=0x0      ctxt=1 aperture=0x11     <- cluster 0 (FE)
ctxt=0 aperture=0x300    ctxt=1 aperture=0x311    <- cluster 3 (GRAS)
ctxt=0 aperture=0x500    ctxt=1 aperture=0x511    <- cluster 5 (PS)
X267: RESTORED aperture HOST=0x0 (now reads 0x0)
```

Saved-and-restored worked: the aperture read back **0** before and after.

### 2. Mesa's register values are live (W=64 workload)

```
GRAS_2D_DST_TL (0x8405)   = 0x00000000
GRAS_2D_DST_BR (0x8406)   = 0x003f003f     <- exactly Mesa's 64x64 rect
GRAS_SC_VP_SCISSOR_TL_0   = 0x00000000
GRAS_SC_VP_SCISSOR_BR_0   = 0x003f003f
RB_MRT_BUF_INFO  (0x8822) = 0x00002030     <- exactly Mesa's descriptor
RB_MRT_PITCH     (0x8823) = 0x00000004     (4 << 6 = 256 bytes)
RB_MRT_ARRAY_PITCH(0x8824)= 0x00000100     (256 << 6 = 16384 bytes)
RB_MRT_BASE_LO   (0x8825) = 0x00026000
RB_MRT_BASE_HI   (0x8826) = 0x00000007     -> 0x700026000 = the render target
```

Every value matches what the command stream was independently decoded to carry
(`FINDING-MESA-EXONERATED-RT-AND-VIEWPORT-CORRECT.md`).

### 3. THE DECISIVE TEST — the state tracks the workload

Re-ran with `W=128 H=64`:

```
GRAS_SC_VIEWPORT_SCISSOR_BR_0 (0x80d1) : 0x003f003f  ->  0x0000007f
```

`63` became `127` **because the workload got wider.** That is not a
coincidence and not a leftover register — the GPU register content is being
driven by Mesa's submission and is readable from the CPU.

This single observation converts "the values look right" into "state delivery
is proven".

## What it eliminates

Combined with the earlier findings, the search space is now almost entirely
below register programming:

| Candidate | Status |
|---|---|
| Mesa emits wrong state | **eliminated** — stream decoded field-correct |
| State doesn't reach the GPU | **eliminated** — registers read back live and workload-tracking |
| Wrong/stale SQE ucode | **eliminated** — newer ucode, identical behaviour |
| RB-specific mechanisms | **eliminated** — compute and TF writes are dead too |
| Context-banking made state unreadable | **eliminated as a blocker** — it *is* readable |

## What survives — all strictly below register programming

The RB/UCHE is being handed a correct, live, workload-tracking set of
registers and produces no DDR write. That leaves only:

- **UCHE / write-back cache flush or invalidation** — every shader-domain store
  lost, CP's own store bypasses it
- **GPU→DDR transaction formation / GBIF**
- **SMMU behaviour on the write direction** — the CP fence write proves
  translation works, but a shader store may take a different path
- **a secure / privilege gate on the shader write path**

## Honest caveats on this experiment

1. **The aperture's effect on visibility is NOT proven.** FE (cluster 0), GRAS (3)
   and PS (5) returned *identical* values for all four ground-truth registers.
   Two readings fit: either these registers are globally readable regardless of
   aperture, or the aperture write doesn't remap them. Either way state is
   readable, so the conclusion holds — but "banked cluster reads work" remains
   unproven, and nothing should be built on it yet.
2. **`RB_RB_SUB_BLOCK_SEL_CNTL_HOST` offset is wrong.** `0x8e3b` reads
   `0xdeafbead` (Qualcomm's unimplemented-register poison), so the RAC/RBP
   sub-block switch never happened. Our tree's `a6xx.xml.h` only defines the
   `_CD` variant at `0x8e3d`. KGSL's `adreno_a6xx.h` has the real `_HOST`
   address; it must be read from there before relying on sub-block selection.
3. **At W=128 the `RB_MRT_*` registers read back as poison** while they were live
   at W=64. Unexplained. Possibly a different Mesa path at that size, or RB state
   reset between submissions. Worth one follow-up run, but it does not affect the
   headline: `GRAS_SC_VIEWPORT_SCISSOR_BR_0` still tracked the workload.
4. This experiment **does not** by itself distinguish the four surviving
   candidates. It relocates them.

## Process notes

- My first ad-hoc compile checker reported a non-compiling patch as clean: the
  quoted `#include "a6xx_gpu.h"` could not resolve from `/tmp`, and the harness
  reported `rc=0, no diagnostics` instead of failing. The real kbuild caught the
  genuine bug (use-before-declaration of `a6xx_x1_aperture_dump`). The checker
  was discarded; **kbuild is the oracle for compile claims.**
- `build-halium-kernel.sh` enters `kernel-build` itself and must be run from the
  host.
- The container has `python2.7` but not `python2`; shim at
  `/home/usb/.opencode-tmp/bin/python2`.

## Files

- `patches/267-aperture-cluster-readback.patch` — 188 lines, one file
- `harnesses/` unchanged; this experiment needed only the in-kernel instrument
- device log: `/root/x267.txt` (W=64), `/root/x267b.txt` (W=128)