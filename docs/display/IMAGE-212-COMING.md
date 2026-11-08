# Image 212 — decode WHICH interrupt the submit produced

Date: 2026-11-11. Built, packaged, **not yet flashed**. Measurement only.

## The consultants, and what they changed

Two researchers ran in parallel. They disagreed with each other, and the
disagreement is the useful part.

### Consultant 1: my leading hypothesis is WRONG (confirmed — I have retracted it)

**v4.19 has no `a6xx_set_pagetable()`**, and v4.19's `a6xx_submit()` is
byte-for-byte the same shape as ours. The mechanism was added in **v5.10**
(commit `84c31ee16f90`, *"Add support for per-instance pagetables"*), and on
current master it emits **nothing at all** unless the Qualcomm `adreno_smmu`
implementation is in use — a vanilla `arm-smmu-v3` GPU domain takes the silent
early-return. CAF agrees: `_set_pagetable_cpu()` writes the arm-smmu context-bank
registers from the CPU; `CP_SMMU_TABLE_UPDATE` is only for per-process page
tables and preemption.

It also kills the theory on the evidence: `RPTR` is advanced as the CM3
**retires** fetched packets, so `RPTR == WPTR` is positive evidence that
translation works and the stream is being decoded. A bad TTBR would stall the
first fetch.

See `RETRACTION-SMMU-TABLE-UPDATE.md`.

### Consultant 2: `memptrs_iova == 0` is very likely the actual root cause

With `memptrs_iova == 0`, `rbmemptr(ring, fence) == 0x4` — **not a mapped GPU
VA**. Under the SMMU that is an AHB/TSB write error, and on stall-on-fault the
CP halts there. That accounts for every symptom:

- **fence never updates** → hangcheck lockup
- **`CP_SCRATCH_REG(2) == 0`** → in a v4.19 stream the CACHE_FLUSH_TS is the
  *first* memptrs-based write (there is no pagetable packet before it), so the CP
  halts with the scratch write already behind it
- **`RPTR == WPTR`** → the SQE prefetches the whole stream into the ROQ before
  executing, so "fetched everything" and "executed nothing" are compatible
- **`RB_RPTR_ADDR == 0x0`** → programmed from `rbmemptr(rptr)` = iova 0

Also **confirmed: `CP_SCRATCH_REG` IS readable on A6XX.** The a630 CP-protect
table deliberately leaves `0x883..0x88a` unprotected, and upstream's
`a6xx_fault_handler()` reads scratch 4–7. So our `== 0` is a genuine signal, not
an artefact — which removes the last "false signal" excuse.

And a **second independent explanation for `RB_RPTR_ADDR == 0`**: if
`has_whereami` is false (old SQE), upstream's `hw_init()` never writes
`CP_RB_RPTR_ADDR` at all because `shadow_bo` is never allocated.

### Also refuted (again, independently)

`RB_MASTER_CONTROL` / `FORCE_PUSH` / any doorbell after WPTR — **no**. Neither
upstream nor CAF writes anything but `CP_RB_WPTR`. And `(1<<31)` is confirmed as
`CP_EVENT_WRITE_0_IRQ`.

### A real divergence worth noting (NOT applied here)

`RB_CNTL = 0x20c` is missing bit 27 (`NO_UPDATE`). Both references write
`0x0800020C` for A640:

```c
/* upstream a6xx_hw_init() */
if (adreno_gpu->base.hw_apriv)
	gpu_write(gpu, REG_A6XX_CP_RB_CNTL, MSM_GPU_RB_CNTL_DEFAULT);
else
	gpu_write(gpu, REG_A6XX_CP_RB_CNTL,
		MSM_GPU_RB_CNTL_DEFAULT | AXXX_CP_RB_CNTL_NO_UPDATE);
```

CAF KGSL agrees: `A6XX_CP_RB_CNTL_DEFAULT | (APRIV ? 0 : (1 << 27))`. **Deliberately
not changed in this image** — one hypothesis per image, and it is plausibly a
*consequence* of `memptrs_iova == 0` rather than a cause.

## What this image adds

`CPINT`, in `a6xx_submit()` after `a6xx_flush()`, first three submits, after a
50 ms delay so the CP can retire:

```
CPINT[seq=N] RBBM_INT_0_STATUS=0x… INT_0_MASK=0x… CP_INTERRUPT_STATUS=0x… CP_SQE_STAT_DATA=0x…
CPINT[seq=N] decodes: cache_flush_ts=… hang_detect=… ahb_err_rb=… | opcode=… ucode=… hw_fault=… prot=… ahb_err_cp=… vsd=… illegal=…
CPINT[seq=N] scratch2=0x… fence=… rptr=… RB_RB_WPTR=0x… RB_RB_RPTR=0x…
```

Plus everything from 211: `RINGCPU` ring dwords, and `auto-rest`/`auto-post`
still removed.

## Why this is the decisive measurement

We have been counting IRQs without ever knowing **which bit** fired, so "one IRQ
per submit" has been ambiguous between success and failure this entire time.

| result | conclusion |
|---|---|
| `ahb_err_cp=1` or `ahb_err_rb=1` | **`memptrs_iova == 0` confirmed as root cause.** The fix is to make the aspace stop handing out IOVA 0 (or reserve the GMU window). Next image. |
| `cache_flush_ts=1`, `scratch2` non-zero | the submit actually succeeded; the fence problem is elsewhere |
| `hang_detect=1` | the CP is reporting a hang, not completing a timestamp — reframes everything |
| `opcode=1` / `ucode=1` | the SQE ucode is choking on the stream — wrong `a630_sqe.fw` for A640 |
| `prot=1` | `a6xx_set_cp_protect()` is blocking our register writes |
| all zero, `scratch2` still 0 | the CP is not retiring at all — back to the ring contents |

## Run procedure

1. Flash, boot. Do not touch the GPU.
2. Trigger **once**, own short command, no retry.
3. Confirm `BABEFACE`, then **bank dmesg to the host**.
4. Run `fbprobe.py` to force a real submit.
5. Bank dmesg again; read `CPINT` and `RINGCPU`.

Gate: `GL_RENDERER == FD640`.

## Verification

- Build exit 0, no new warnings.
- Byte-identical to 211 in header (all 10 fields), cmdline, ramdisk, size.
  (First packaging attempt had a typo in `--tags_offset` and was discarded.)

```
artifacts/flash/212-halium-boot-a6xx-cpint.img
sha256 9594d51bd42d771c30c948d8417ee90b57baac0ec7f29b155691c2fdb389777e
```
