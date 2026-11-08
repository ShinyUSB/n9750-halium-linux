# FINDING: the CP executes userspace IBs — and its writes to user buffers are discarded

Date: 2026-10-20
Device: d2q (SM8150, Adreno 640), image 276 (`276-halium-boot-a6xx-hfilayout.img`,
sha256 173f7c2f8949487febbd2744a3f08e9b2c5ff4c266481f4f1106b34e230067d0).
Harnesses: `/home/usb/.opencode/probe_cp_exec.py`, `/home/usb/.opencode/secvid_cp.py`.

## The measurement

The CP was asked, from a **userspace** command stream, to write a magic value
into the memptrs' `rptr` word:

```
CP_EVENT_WRITE(0x80000004, addr=0x700000000, value=0x5A5A1234)
```

using exactly the packet the driver itself emits (`a6xx_gpu.c:1765-1769`) and
with the PM4 parity bits this kernel's `OUT_PKT7` requires.

Result:

```
[  348.763317] CPINT[seq=1] scratch2=0x00000000 fence=0 rptr=1515852340
                                        rptr = 0x5A5A1234  <- OUR VALUE
```

**The CP fetched, executed and completed a command stream that lives in a
userspace BO, and the write landed in DDR.** This is the first positive
proof in the project that the CP runs anything the host put in a userspace
buffer.

## What that rules out, and what it pins down

* **H2 is dead.** "The CP never fetches a userspace IB" is refuted. Every
  earlier observation that looked like "the GPU wrote nothing" was in fact
  "the GPU wrote nothing *to the place we were looking*", or was taken on a
  CP that was not executing at all.
* The whole "the GPU never touches the buffer" class of evidence is
  re-interpretable: the writes were *executed* and then discarded.

And with a live CP, the *other* measurement becomes meaningful for the first
time (it was void on images 277/278, where the CP executed nothing):

```
[A] CP_MEM_WRITE(tiova=0x700011000, 0x11111111)   -> A_WROTE=False
```

So, on one and the same live CP, in one boot:

| the CP writes to | lands? |
|---|---|
| `0x700000000` — the memptrs page, a **driver** BO | **yes** (0x5A5A1234 read back) |
| `0x700011000` — a **userspace** BO | **no** (buffer still zero) |

Meanwhile, for the failing write:
* the SMMU logged **zero** faults (no "Unhandled context fault", no
  "No Device SMMU Fault") across the whole run;
* the page table maps it (PTRMAP: `bo[0] dword 3 -> iova 0x700011000`);
* the CP consumed the ring (RPTR caught up to WPTR) and retired the submit.

**So the transaction is translated by the MMU and authorised, executed by the
CP, and then dropped between the GPU's memory client and DDR.** The gate is
not the SMMU, not the command stream, and not the CP.

## The remaining difference: driver BO vs userspace BO

Both writes are `MSM_BO_WC` (the memptrs are allocated `MSM_BO_WC` at
`adreno_gpu.c:633`, our test buffer is `MSM_BO_WC`), same context bank, same
command-stream mechanism, same page-table format. What differs:

1. **address range** — `0x700000000` (the base of the GPU IOVA window, where
   the driver puts its boot-time buffers) vs `0x700011000` (the first
   userspace allocation);
2. **allocation path** — `msm_gem_kernel_new()` vs the userspace
   `DRM_IOCTL_MSM_GEM_NEW` + mmap path, i.e. a different DMA-mapping path;
3. **physical region** — the pages the two buffers live in may sit in
   different physical ranges, and the secure world may protect one of them.

(3) is the one with a plausible mechanism: a NoC/XPU policy that filters GPU
writes by *physical* range would allow the driver's own pages and refuse
userspace pages — and it would also explain the 167/213 archived
`TZBSP_ERR_FATAL_NOC_ERROR` EL2 aborts, and why the same GPU works under
Android/KGSL (different allocation pattern).

## Two other results from images 277/278 (and why they are void as "write" evidence)

* **Secure mode is closed negative.** With the ZAP loaded and running
  (`pil-a640_zap: active_count(1)`), the kernel's `CP_SET_SECURE_MODE(0)`
  packet was consumed (RPTR 0x9 -> 0xb) and `TRUST_CNTL` did **not** move
  (0x1 -> 0x1). `TRUST_CNTL=0x1` is not the smoking gun the project assumed.
* **The SMMU aperture handshake is not missing.** `scm_call2(SCM_SVC_MP,
  CP_SMMU_APERTURE_ID)` is *available* and TZ *accepts* it (`avail=1 ret=0`,
  cb=0, arg0=0xffff0000) — the same call stock KGSL makes — and it changed
  nothing. Closed negative.
* **But image 277's packet is not a no-op: it stops the CP executing.**
  In 277/278 the driver's own writes show `scratch2=0x00000000 fence=0`,
  while image 276 (with and without the ZAP) shows `scratch2=1,2 fence=1,2`.
  The CP keeps *fetching* but executes nothing. Every "the write did not
  land" measurement taken on 277/278 was therefore taken on a dead CP.

  This is worth keeping: a single CP packet can put the GPU into a
  fetch-but-don't-execute state. It is a lever, and it may be the same state
  the GPU is in for a different reason.

## Next experiment (proposed)

One kernel image that settles (1) vs (2) vs (3):

* a debugfs/sysfs entry that takes an IOVA and issues a `CP_MEM_WRITE` to it
  **from the driver** (not from userspace), and reports the read-back;
* the same entry printing, for a driver BO (memptrs) and a userspace BO:
  IOVA, **physical address**, and the SMMU PTE attributes;
* the write test repeated against a **kernel** BO allocated at a *high* IOVA
  (i.e. same kind of object, different address) and against a **userspace**
  BO at a *low* IOVA (same range, different kind of object).

That matrix separates "which address ranges the GPU may write" from "which
kinds of object it may write" — and the physical address of the two buffers
tells us whether the secure world is filtering by physical range.

## Method notes (for the next session)

* **Always check the driver's own `scratch2`/`fence` before trusting any
  write measurement.** If `fence` is not advancing, the CP is not executing
  and every result is void. This precondition is now printed by
  `run276probe.sh` before the measurements.
* PM4 type-7 headers need the parity bits: `(7<<28) | cnt |
  (parity(cnt)<<15) | (opcode&0x7f)<<16 | (parity(opcode)<<23)`. The
  harness asserts this against the project's known-good `CP_DRAW` header
  (`0x70388003`). The earlier harness omitted them, which voided two runs.
* The sync-object "fence retired" from the submit ioctl is **not** evidence
  that the GPU executed anything; the CPU-visible fence word is.
