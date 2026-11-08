# FINDING: the CP writes to user memory fine — only the 3D domain's writes die

Date: 2026-10-21
Images: 276 (`276-halium-boot-a6xx-hfilayout.img`,
sha256 173f7c2f8949487febbd2744a3f08e9b2c5ff4c266481f4f1106b34e230067d0)
Harnesses: `/home/usb/.opencode/probe_cp_exec.py`, `/home/usb/.opencode/secvid_cp.py`,
`/home/usb/.opencode/nib.py`, `/home/usb/.opencode/nib3.py`.

## The measurement that changes everything

On image 276, with a **healthy** CP (the driver's own fence word advancing:
`scratch2=0x00000004 fence=4 rptr=256`), in a single boot:

```
=== STEP 2: our raw CP_MEM_WRITE, on that same CP ===
    TARGET after A = (286331153, 0, 0, 0)
    A_WROTE=True                       <- 286331153 == 0x11111111
```

**A plain `CP_MEM_WRITE` from a userspace command stream to a userspace
buffer LANDED.** The value 0x11111111 was read back through the CPU mapping.

And, in the same boot, the workload (Mesa) still writes nothing:

```
  VERDICT: GPU WROTE NOTHING - prefill 0xAA fully intact, oracle-validated
  VERDICT: GPU WROTE NOTHING - prefill 0x5A fully intact, oracle-validated
```

So, on one and the same live GPU, in one and the same boot:

| what writes | result |
|---|---|
| **CP** (command processor) → userspace buffer | **WORKS** (0x11111111 landed) |
| **CP** → the memptrs page | **WORKS** (0x5A5A1234 landed, `probe_cp_exec.py`) |
| **SP/UCHE** (shader) → SSBO | fails, 0/64 |
| **RB/2D** (render block, CP_BLIT) → buffer | fails, 0/4096 |
| **RB** → FBO | fails, 0/6 |

**The memory path is not broken. The CPU's write path is not broken. The
defect is specific to the 3D domain's write path** — the shader (UCHE) and
the render block — and every test the project ever ran exercised only those
two, never the CP's.

That is why 260+ images found nothing: the project's oracle could only see
the two clients that are broken, and the two clients that work were never
used as the control.

## How each earlier belief was tested tonight

| belief | test | result |
|---|---|---|
| "the CP never fetches a userspace IB" | `probe_cp_exec.py`: CP writes 0x5A5A1234 into the memptrs' `rptr` word from a userspace IB | **REFUTED** — the value landed |
| "secure mode gates the writes" (`TRUST_CNTL`) | image 277/278: ZAP loaded and running (`active_count(1)`), `CP_SET_SECURE_MODE(0)` issued by the kernel's own `OUT_PKT7` | **REFUTED** — the CP consumed it (RPTR 0x9→0xb) and `TRUST_CNTL` stayed 0x1 |
| "TZ was never told the GPU's SMMU aperture" | image 278: `scm_call2(SCM_SVC_MP, CP_SMMU_APERTURE_ID)` with the CB number from `DOMAIN_ATTR_CONTEXT_BANK` | **REFUTED** — `avail=1 ret=0`, TZ accepted it, nothing changed |
| "the indirect buffer's size is wrong (bytes vs dwords)" | `nib3.py`: size = 4, 16, 64, 256, 1024, 4096 | **REFUTED** — the fence never advanced for any size |
| "our harness's PM4 headers were malformed" | parity bits added, asserted against the project's known-good `CP_DRAW` (`0x70388003`) | **fixed** — this voided two earlier runs |
| "the sync-object 'fence retired' means the GPU executed it" | compared the ioctl's fence fd with the CPU-visible fence word | **REFUTED** — the fd signals from the driver; the GPU's own write is the only real evidence |

## Two findings about the project's own method

1. **The submit ioctl's "fence retired" is not evidence of GPU execution.**
   The CPU-visible fence word (`ring->memptrs->fence`, written by the GPU's
   `CP_EVENT_WRITE`) is. Images 277/278 were measured on a CP that was
   fetching but *not executing* (`scratch2=0x00000000 fence=0`), so their
   results were void. Image 276 with the same driver shows `fence=1,2,3,4`.
2. **Image 277's `CP_SET_SECURE_MODE(0)` packet is not a no-op**: it leaves
   the GPU fetching the ring while executing nothing. The same is true of a
   `CP_EVENT_WRITE` with the `CACHE_FLUSH_TS` event id. Both are powerful
   levers and both are easy to mistake for "the GPU is broken".

## Where the defect now lives, and the next experiment

The UCHE — the shader's memory client — has its **own** address filters, and
the working driver says they belong at zero:

```c
/* drivers/gpu/msm/adreno_a6xx.c — a6xx_pwrup_reglist[] (CAF) */
{ A6XX_UCHE_WRITE_RANGE_MAX_LO,  0x0 }, { A6XX_UCHE_WRITE_RANGE_MAX_HI,  0x0 },
{ A6XX_UCHE_TRAP_BASE_LO,        0x0 }, { A6XX_UCHE_TRAP_BASE_HI,        0x0 },
{ A6XX_UCHE_WRITE_THRU_BASE_LO,  0x0 }, { A6XX_UCHE_WRITE_THRU_BASE_HI,  0x0 },
```

We instead write upstream msm's values (a6xx_gpu.c, "Disable L2 bypass in
the UCHE"):

```
WRITE_RANGE_MAX = 0x0001ffff_ffffffc0
TRAP_BASE       = 0x0001ffff_fffff000
WRITE_THRU_BASE = 0x0001ffff_fffff000
```

The project's audit compared these registers against **upstream** (we match)
and never against **CAF** — the one driver known to work on this exact part
under the same TZ. Their names say what they are: a *write-range limit* and
a *trap base* for the shader client, while our IOVAs live at 0x700000000.

**Image 279** adds `a6xx.uche_filter_clear=1`, which writes CAF's zeros into
all six registers and reads them back, and otherwise changes nothing. Base
image 276 (the last one with a healthy CP). If the shader's writes start
landing, the project has its root cause; if not, the filters are excluded
too and the next candidate is the per-client GBIF configuration for the
non-CP clients.
