# Image 213 — fix the ring dump, and ask where the SQE actually is

Date: 2026-11-11. Built, packaged, **not yet flashed**. Measurement only.

## What image 212 established

The first genuinely decisive measurement of the project:

```
CPINT[seq=0] RBBM_INT_0_STATUS=0x00000000 CP_INTERRUPT_STATUS=0x00000000
CPINT[seq=0] decodes: cache_flush_ts=0 hang_detect=0 ahb_err_rb=0 |
             opcode=0 ucode=0 hw_fault=0 prot=0 ahb_err_cp=0 vsd=0 illegal=0
CPINT[seq=0] scratch2=0x0 fence=0 rptr=114 WPTR=0x72 RPTR=0x72
```

Two conclusions, one of them refuting a consultant's theory:

**1. The CP is not faulting — it is executing nothing.** Every interrupt status
register is zero: no AHB error, no hang detect, no opcode error, no ucode error,
no register-protection error. This is an *absence of execution*, not a failure.

**2. `memptrs_iova == 0` is NOT the bug.** `rptr=114` is the GPU having written
the RPTR shadow into `memptrs->rptr` — i.e. IOVA 0 is a valid, GPU-writable
address. Consultant 2's "the fence write to IOVA 0x4 faults" theory is
**refuted by the measurement it predicted**. See
`RETRACTION-SMMU-TABLE-UPDATE.md` for the parallel refutation of consultant 1.
Both hypotheses are now dead, and the fault is neither address translation nor
page tables.

**3. Nothing has ever executed on this GPU — including bring-up.** The ring still
contains the `CP_ME_INIT` packet written by `a6xx_cp_init()` during hw_init:

```
[090] = 0x70c80008   PM4 header, opcode 0x70 count 8
[091] = 0x0000002f   }
[092] = 0x00000003   |  8 CP_ME_INIT payload dwords
[093] = 0x20000000   }
[094..098] = 0      }
```

Still sitting unretired. `BABEFACE` is the CM3/GMU — a separate processor — so
bring-up success never implied the CP was alive. The SQE was never initialised,
which is why nothing retires.

## A bug in my own diagnostic, found and fixed

Image 209/211/212's `RINGCPU` printed `j` from `cur-16` to `cur` — dwords
**below** the commit point, i.e. the *previous* content. That is why image 212
captured the leftover `CP_ME_INIT` instead of the stream we had just written.
`ring->cur` is not advanced until `a6xx_flush()`, which runs after the print.

Fixed to print `cur .. next` — the actual stream this submit. The header now also
reports `ndwords` so the count is checkable at a glance. (`cur=99 next=114`
already confirmed the 15-dword stream is `3+3+3+6` with zero indirect buffers,
but we never had its bytes.)

## What this image adds

**1. Corrected `RINGCPU`** — the real submitted packets.

**2. `SQE213`** — identify the microcode actually loaded, at `a6xx_ucode_init()`:

```
SQE213: name=a630_sqe.fw size=… b0=0x?? b2=0x?? ver=0x??? nib0=0x? nib2=0x? would_whereami=?
```

The nibbles implement upstream's `a6xx_ucode_check_version()` gate, which our
tree never calls:

```c
if ((((buf[0] & 0xf) == 0xa) && (buf[2] & 0xf) >= 1) ||
    (buf[0] & 0xfff) >= 0x190)
	a6xx_gpu->has_whereami = true;
```

`has_whereami` gates whether upstream allocates `shadow_bo` and therefore whether
`CP_RB_RPTR_ADDR` is programmed at all — an independent explanation for
`RB_RPTR_ADDR == 0x0` that does not involve memptrs.

**3. `CP_SQE_STAT_ADDR` / `CP_SQE_STAT_DATA`** plus `RBBM_STATUS`, `SQE_CNTL`,
`SQE_BASE` — where the sequencer actually is: idle, busy, waiting on a timestamp,
or wedged on an error.

## Decision rule, declared BEFORE flashing

| observation | conclusion |
|---|---|
| `would_whereami=0` and SQE stat shows idle/error | wrong SQE generation for A640, or the version check our tree omits matters. Next image swaps the blob or backports the check. |
| SQE stat shows it *waiting on a timestamp* | the SQE started but is blocked — look at what it is waiting for, likely the missing `CP_ME_INIT` ordering. |
| SQE stat idle with `SQE_CNTL=1` and a valid `SQE_BASE` | the SQE was never started at all — the enable path, not the firmware. |
| ring dwords are correct packets (not what 212 showed) | confirms the dump fix; stream is fine, so the fault is below it. |
| ring dwords are garbage/zeros | ring memory really is stale — the coherency question, reopened with correct data. |

## Run procedure

1. Flash, boot, do not touch the GPU.
2. Trigger **once**, own short command.
3. Confirm `BABEFACE`, bank dmesg.
4. `python3 /usr/local/bin/glstage.py 5` (fbprobe alone does not submit).
5. Bank dmesg; read `SQE213`, `RINGCPU`, `CPINT`.

Gate: `GL_RENDERER == FD640`.

## Verification

- Build exit 0, no new warnings.
- Byte-identical to 212 in header (all 10 fields), cmdline, ramdisk, size.

```
artifacts/flash/213-halium-boot-a6xx-sqe.img
sha256 9c71523568075e29f3fa342565289235628ade4293dec23b420541c4ff106240
```
