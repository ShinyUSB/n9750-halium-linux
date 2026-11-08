# Image 213 — CPU side is proven correct; the SQE runs but retires nothing

Date: 2026-11-11. Captures: `captures/213-post.txt`.

## The ring contents are correct

The `RINGCPU` window fix worked — this is the actual submitted stream, `cur..next`,
15 dwords:

```
[099] 0x70460001   CP_EVENT_WRITE hdr      [100] 0x00000018  PC_CCU_INVALIDATE_DEPTH
[101] 0x70460001   CP_EVENT_WRITE hdr      [102] 0x00000019  PC_CCU_INVALIDATE_COLOR
[103..104]         OUT_PKT4 CP_SCRATCH(2) + payload
[105..108]         CP_INDIRECT_BUFFER_PFE + iova_lo + iova_hi + size   <- Mesa DID submit
[109] 0x70460004   CP_EVENT_WRITE hdr
[110] 0x80000004   CACHE_FLUSH_TS | (1<<31)      ✓
[111] 0x00000004   low32(rbmemptr(ring,fence))   ✓  (memptrs_iova 0 + 4)
[112] 0x00000000   high32                        ✓
[113] 0x00000000   submit->seqno                 ✓
```

15 = 2 + 2 + 2 + 4 + 5. **Mesa submitted an indirect buffer**, so there is real
rendering work in the stream. The command buffer the CPU produced is exactly
what `a6xx_submit()` should emit.

**This closes the coherency family for good.** The ring is written correctly,
`CP_RB_BASE` points at it, the GPU fetches it and mirrors RPTR into memptrs
(`rptr=114`). Nothing is wrong on the CPU side or with address translation.

## Both earlier hypotheses are dead, killed by measurement

- `memptrs_iova == 0`: the GPU wrote `rptr=114` into IOVA 0. IOVA 0 is valid and
  GPU-writable, so "the fence write to IOVA 0x4 faults" is refuted by the very
  measurement that predicted it.
- Missing `CP_SMMU_TABLE_UPDATE`: refuted on version grounds — v4.19 never had it.

## What the hardware is actually doing

```
RBBM_INT_0_STATUS   = 0x00000000
CP_INTERRUPT_STATUS = 0x00000000
  cache_flush_ts=0 hang_detect=0 ahb_err=0 opcode=0 ucode=0 hw_fault=0 prot=0
scratch2=0  fence=0  rptr=114  WPTR=0x72  RPTR=0x72
SQE_STAT_ADDR=0x00000001  SQE_STAT_DATA=0x70460004  RBBM_STATUS=0x1
SQE_CNTL=0x1  SQE_BASE=0x9000
```

- **Not a fault.** Every error bit is zero.
- **The SQE is alive and progressing.** `CP_SQE_STAT_DATA` changed from `0x12e`
  (read immediately after the flush) to `0x70460004` — which is *exactly the
  header of the CACHE_FLUSH_TS packet at ring[109]*. It fetched the whole stream
  and is sitting on the final packet.
- **And yet it retires nothing.** `scratch2` is 0, `fence` is 0, and
  `CACHE_FLUSH_TS` never fires — the packet it is parked on is the one that
  would raise the interrupt.

So: the sequencer fetches, decodes nothing, executes nothing, and reports no
error. That is the behaviour of a microcode that cannot make sense of the
stream it is being fed.

## The SQE firmware is now the prime suspect

```
SQE213: name=a630_sqe.fw size=32304 b0=0x00 b2=0x00 ver=0x000 nib0=0x0 nib2=0x0 would_whereami=0
```

and on disk:

```
/lib/firmware/a630_sqe.fw : 00 00 00 00 | 90 e1 6e 01 0b 1f 00 01 00 00 00 01 01 00 02 88 ...
```

Upstream's gate:

```c
if ((((buf[0] & 0xf) == 0xa) && (buf[2] & 0xf) >= 1) ||
    (buf[0] & 0xfff) >= 0x190)
	a6xx_gpu->has_whereami = true;
```

`buf[0] = 0x00` fails both limbs, so `has_whereami = false` — i.e. this blob is
not the SQE generation upstream expects for a630/a640. Upstream never runs
that check in a v4.19-era tree, which is why nothing complained.

`has_whereami` also independently explains `CP_RB_RPTR_ADDR == 0x0`: upstream
only programs it when `shadow_bo` exists, and `shadow_bo` requires
`hw_apriv || has_whereami`.

## Cumulative state

| question | status |
|---|---|
| bring-up | **solved** — reliable across many boots, `BABEFACE`, 7/7 HFI ACKs |
| Mesa submits reach the kernel | **solved** — `submitted` counter, and now a real IB in the stream |
| ring contents correct | **solved this image** — 15 well-formed dwords |
| address translation | **solved** — `rptr` written at IOVA 0 |
| is it faulting? | **solved** — no, every error bit is zero |
| **why does the SQE retire nothing** | **open — now almost certainly the firmware** |

## Next step

Verify the provenance of `a630_sqe.fw` on this image and compare against a
known-good blob for A640:

1. Check both copies (`/lib/firmware/` and `/lib/firmware/qcom/`) and
   `/usr/lib/firmware/` — are they identical, and which does the loader pick?
2. Establish what a genuine `a630_sqe.fw` header looks like (should start
   `0x0a…` per the `buf[0] & 0xf == 0xa` gate). Ours starts `00 00 00 00`.
3. If the blob is wrong or truncated, obtain the correct one and retry.

Only after that is it worth revisiting `CP_ME_INIT` ordering or any execution
path change — with a correct SQE the rest of the stack is now proven sound.
