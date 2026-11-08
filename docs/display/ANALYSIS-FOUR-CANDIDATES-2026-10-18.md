# ANALYSIS — four candidates, source-only. No hardware probing.

Date: 2026-10-18
Baseline: image 267, newer SQE ucode installed.
Constraint: software-only. No register writes, no new kernel, no flash.
Author: main session.

---

## 0. Headline

One of the four candidates is **not merely untested — it was designed, wired up,
and then silently never run**, because of a cmdline-truncation bug. It is also
the only one of the four whose mechanism matches the observed failure signature
exactly.

Everything else in this document is secondary to that.

---

## A. UCHE / cache flush — **ELIMINATED**

### Mechanism it would have to explain
Shader-domain stores sitting in a write-back cache that is never pushed to DDR,
while the CP's own stores bypass that cache. Would produce exactly: registers
live, no fault, CPU always sees its prefill.

### What the source says
Cache maintenance on a6xx is emitted as `CP_EVENT_WRITE`, not a dedicated opcode:

- `fd6_event_write()` — `fd6_emit.h:151`; header `0x7046_000N`, `payload[0]` = event id
- `fd6_cache_flush()` — `fd6_emit.h:179`:
  `RB_DONE_TS(TS)` → `CP_WAIT_REG_MEM` → `CACHE_FLUSH_TS(TS)` → `CP_WAIT_MEM_GTE`
- `fd6_cache_inv()` — `fd6_emit.h:171`: invalidate only (color, depth, CACHE_INVALIDATE)
- `fd6_framebuffer_barrier()` — `fd6_emit.c:1412`:
  `RB_DONE_TS` → wait → `PC_CCU_FLUSH_COLOR_TS` + `PC_CCU_FLUSH_DEPTH_TS` →
  `CACHE_FLUSH_TS` → `fd_wfi` → `CACHE_INVALIDATE`

Call sites: `fd6_cache_flush` is used by **compute** (`fd6_compute.c:203`) and
**GMEM tile-render** (`fd6_gmem.c:746`) only. Everything else calls
`fd6_cache_inv` only. The post-draw flush that matters is
`fd6_draw.c:447` — a bare timestamped `CACHE_FLUSH_TS`.

### What the captured streams say
Traced with `harnesses/evtrace.py` over the surviving captures:

| capture | envelope events found |
|---|---|
| `m-sysmem.txt` | `PC_CCU_INVALIDATE_COLOR`, `CACHE_INVALIDATE`, **`PC_CCU_FLUSH_COLOR_TS`**, **`PC_CCU_FLUSH_DEPTH_TS`**, **`CACHE_FLUSH_TS`×3 (all TS)** |
| `nd-r64.txt` | `PC_CCU_INVALIDATE_COLOR`, `CACHE_INVALIDATE`, **`CACHE_FLUSH_TS`×2 (TS)** |
| `nd-e11.txt` | `PC_CCU_INVALIDATE_COLOR`, `CACHE_INVALIDATE`, **`CACHE_FLUSH_TS`×3 (TS)** |

Ordering, in `nd-e11.txt`: the 11 `CP_DRAW`s live in the **leaf** buffer `bo[7]`
at dwords 69–109; the flush events live in the **envelope** `bo[10]` at 327/336/341.
`bo[7] < bo[10]` in the kernel's cmd list, so the ring dispatches the draws
**before** the flush. Order is correct.

Also verified as correctly programmed: `GRAS_DBG_ECO_CNTL` is written `0x880`
(reg `0x8600` in the stream), `HLSQ_DBG_ECO_CNTL` `0x80000` (reg `0xbe04`),
`TPL1_UNKNOWN_B605` `0x44` (reg `0xb605`), `HLSQ_UNKNOWN_BE00` `0x80` (reg `0xbe00`),
`SP_CHICKEN_BITS` `0x1430` (reg `0xae03`) — the whole `fd6_emit_restore()` ECO
family lands as Mesa intends. (These registers have **no name in any public XML**;
they were identified by correlating the write sequence against `fd6_emit.c:1266-1279`.)

### Verdict
**A flush is present, timestamped, and correctly ordered after the draws.** A
missing or mis-ordered flush is not supported. **Confidence: high.**

Caveat: `CP_WAIT_REG_MEM` (op `0x3c`) does not appear anywhere, so the *polling*
variant of `fd6_cache_flush()` is not in use — only the bare timestamped flush.
We cannot confirm from a static capture that the flush *completed*, only that it
was ordered before the fence. Residual risk: low.

---

## B. GBIF / `UCHE_GBIF_GX_CONFIG` — **TOP LEAD, AND IT HAS NEVER BEEN RUN**

### Mechanism
`A6XX_UCHE_GBIF_GX_CONFIG` limits the UCHE's GBIF read/write path. If UCHE's
write path to DDR is mis-configured, every **shader-domain** store (RB render
stores *and* SP compute stores) can be dropped silently — while the CP's own
store, which does not traverse UCHE, continues to work. That is our exact
signature: no faults, registers live, no DDR write from SP or RB, fence writes fine.

### What the vendor does
```
/home/usb/ShinyKernel/Kernel/drivers/gpu/msm/adreno_a6xx.c:804-805
    if (ADRENO_QUIRK(adreno_dev, ADRENO_QUIRK_LIMIT_UCHE_GBIF_RW))
        kgsl_regwrite(device, A6XX_UCHE_GBIF_GX_CONFIG, 0x10200F9);
```
- `A6XX_UCHE_GBIF_GX_CONFIG = 0xE3A` — `a6xx_reg.h:753`
- `ADRENO_QUIRK_LIMIT_UCHE_GBIF_RW = BIT(8)` — `adreno.h:159`
- quirk table entry — `adreno.c:773`

### What our tree does
The write is **deliberately deferred**:
```
a6xx_gpu.c:2395-2401
  "KGSL limits UCHE GBIF read/write via A6XX_UCHE_GBIF_GX_CONFIG = 0x10200F9
   ... but gates it on ADRENO_QUIRK_LIMIT_UCHE_GBIF_RW, which is a DT quirk we
   do not know the value of. Default OFF rather than invent a condition; flip
   with a6xx.uche_gbif_rw=1 to test it."
```
and it is already wired as a parameter:
```
a6xx_gmu.c:3508-3510
  module_param(uche_gbif_rw, bool, 0444);
  "1=write UCHE_GBIF_GX_CONFIG=0x10200f9 (default 0; KGSL gates this on an
   unknown DT quirk)"
```
`0444` ⇒ command-line only. Register address is present in our XML
(`REG_A6XX_UCHE_GBIF_GX_CONFIG 0x00000e3a`, `a6xx.xml.h:558`).

### Why it has never been tested
`IMAGE-243-PARITY-BUILD.md`:
> "Image 242's cmdline is 535 chars in a 512-byte field. The original builder did
> not bound-check and let it **overflow into the `id` field at offset 576**, so the
> trailing `d=1 a6xx.uche_gbif_rw=1` was never delivered to the kernel at all."

`pack-d2q.sh:64` now does `head -c 512`, deliberately reproducing the truncation.
Verified on the running device — `/proc/cmdline` ends mid-token:

```
... a6xx.force_gx_clk=0 a6xx.send_a androidboot.verifiedbootstate=orange ...
```

So the flag was never in effect on any image built so far. **This experiment has
never executed.**

### Verdict
**Highest-value remaining action.** It is a single already-implemented,
vendor-blessed register write behind an existing parameter. No new
instrumentation, no register probing, no new failure mode. **Confidence that it
is worth testing: high. Confidence that it is the root cause: unknown — but it is
the only candidate whose mechanism explains the SP+RB commonality without
requiring a fault.**

---

## C. SMMU write path — **WEAK**

- The driver already tests for the relevant condition:
  `A6XX_RBBM_STATUS3_SMMU_STALLED_ON_FAULT` — `a6xx_gpu.c:3002`
- Every capture shows `CP_INTERRUPT_STATUS = 0` and `int0 = 0`; no SMMU stall, no fault.
- `memptrs->fence` increments 0→1→2, proving IOVA translation and CP stores work.
- Device-tree IOMMU / stream-ID configuration was **not** re-audited in this pass.
- **Not fully excluded:** a page-table entry lacking write permission would not
  fault — it would silently drop the store. That path is unexamined.

**Confidence: low-medium.** Would require auditing GEM attach / IOVA allocation
and page-table permission setup. Lowest information per unit of effort right now.

---

## D. Secure gate — **WEAK**

- Our tree programs protection: `CP_PROTECT_CNTL = 0x3` and `CP_PROTECT(0..6)`
  — `a6xx_gpu.c:2595-2604`
- The driver decodes `CP_REGISTER_PROTECTION_ERROR` — `a6xx_gpu.c:1697`
  — **never observed set** in any capture
- KGSL emits `CP_SET_SECURE_MODE` (`adreno_ringbuffer.c:488`); our tree has no
  secure path (`HWINIT[15] secure-submit: no secure path in this tree`)

Secure mode is presumably never engaged, so it should not gate writes.

**Confidence: low.** No evidence for it; retained only because it is cheap to
keep off the list.

---

## Summary

| # | Candidate | Status | Confidence | Action |
|---|---|---|---|---|
| A | UCHE / cache flush | **eliminated** — present, timestamped, ordered after draws | high | — |
| B | GBIF `UCHE_GBIF_GX_CONFIG` | **never tested** — cmdline truncation | high priority | **test** |
| C | SMMU write permission | open, weak | low-med | defer |
| D | Secure gate | open, weak | low | defer |

---

## Lowest-risk next step

**Enable the existing `a6xx.uche_gbif_rw=1` via the kernel command line, reboot,
re-run the oracle-validated compute probe.**

- No kernel source change, no rebuild of kernel objects, no new instrumentation.
- Only the boot image is repacked (cmdline field) and reflashed BOOT-only.
- Risk profile: one vendor-proven register write already present in our tree
  behind a parameter, gated off by default. It is not an experimental PM4 packet
  or a live-hardware probe, so it does not carry the 264/265/266 or x267 risk class.

### Making room in the 512-byte field
The effective cmdline is already exactly 512 bytes and ends mid-token, so
`a6xx.uche_gbif_rw=1` (23 bytes with separator) must displace something that
*was* delivered. Proposed, in order of increasing risk:

1. Drop `rcupdate.rcu_expedited=1 ` (24 B) and `swiotlb=2048 ` (12 B) → frees 36 B,
   net 512 − 36 + 23 = **499 B**. Both are performance tunables, not functional
   for GPU bring-up. **Recommended.**
2. (More invasive) shorten/drop `video=vfb:640x400,bpp=32,memsize=3072000` (39 B).
   We already avoid the display path in every harness, but this touches console
   fallback.

Must **preserve**: `firmware_class.path=/vendor/firmware_mnt/image` (firmware
loading), `a6xx.gpu_init_delay_ms=300000`, `a6xx.experimental_nonlegacy=1`,
`a6xx.a6xx_selftest=1`, `ramoops.*` (postmortem), `console=ttyMSM0`, `root=`/`init=`.

### How the outcome changes our understanding

- **Compute→SSBO or render starts writing** ⇒ root cause found: UCHE GBIF write
  path. Next step is to determine the correct gating (the DT quirk value) rather
  than leaving an unconditional write, then re-verify with the full matrix.
- **Still dead** ⇒ B is eliminated, and with A eliminated the search collapses to
  C (SMMU page-table permissions) or something below the memory system. That would
  make a permission audit of GEM attach / IOVA allocation the next task — still
  source-only.

---

## Decision on image 268

**268 is not justified. Do not build it.**

Reasons:

1. The investigation it was meant to serve has already produced a cheaper,
   decisive experiment (B).
2. The user's correction is right and I accept it: `CP_RB_RPTR == CP_RB_WPTR` is
   **not** a valid quiescence proof — an empty ring only means no *new* work was
   queued. Upstream's real mechanism is `adreno_gpu_idle()`, which additionally
   waits on the SQE idle-detect bit (`CP_SQE_CNTL_STATUS_IDLE_DETECT`) and the
   fence; KGSL's snapshot path *halts* the GPU before sampling. Neither was
   present in image 267's instrument.
3. The freeze mechanism is understood and should not be re-approached: the
   aperture/sub-block writes were issued against a **live** GPU, and
   `CP_APERTURE_CNTL_HOST` makes the host's address decode aperture-relative, so
   an offset valid in the default aperture can address something else entirely
   once the aperture is switched.

Register probing is **not** permanently abandoned. If a future question genuinely
requires it, the preconditions are: zero GPU register writes; a bounded, reviewed
read-only list; driver-owned quiescence via the real idle mechanism; a hard abort
on idle-check failure; and explicit audit of each register's power-domain and
read-safety. Absent established synchronisation semantics, the investigation stays
software-only.

---

## Correction to the earlier proposed idle precondition

Recorded so it is not silently reintroduced: `rptr == wptr` was suggested as the
guard in the image-267 proposal. It is insufficient. The correct references are
upstream `adreno_gpu_idle()` (ring drain **plus** SQE idle-detect **plus** fence)
and KGSL's halted-GPU snapshot. Mixing DRM/MSM and KGSL APIs without verifying
semantics is exactly the error to avoid.

---

## Files

- `harnesses/evtrace.py` — new. Traces cache/fence events through captured
  command streams. Reports honest limits (no full type-7 length walk; `CP_DRAW`
  and `CP_WAIT_REG_MEM` located by exact value, `CP_EVENT_WRITE` by header prefix).
- `EVIDENCE-INTEGRITY-x267.md` — records that the x267 raw captures were lost and
  precisely which claims are therefore transcriptions.