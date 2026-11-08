# State: the submit is well-formed, the CP executes, and the stream never retires

# SUPERSEDED — see `FINDING-SUBMIT-PATH-PROVEN-SOUND.md`

**The central claim below is WRONG and is retracted.** "The stream never
retires" rested on `cache_flush_ts=0` read 50 ms after the submit. The
image-261 time-resolved burst shows `fence` going `0 -> 1` within ~100 us,
carrying the submit seqno: **the `CACHE_FLUSH_TS` timestamp write landed, so
the whole command stream executed and retired.** The `flush_ts` interrupt bit
is transient and the driver's IRQ handler clears it before any post-hoc read
can see it.

This was the falsification condition pre-registered in
`PLAN-IMAGE-261-TIMED-SAMPLER.md`, and it fired.

The ring-contents, render-target-identification and render-path-matrix sections
below remain sound.

---

Date: 2026-11-16. Supersedes the "SQE never starts" claim in
`ROOT-CAUSE-SQE-NEVER-STARTS.md` (see Retraction 1).

## Retraction 1 — "the SQE is frozen" is NOT established

`a6xx_gpu.c:1262-1263`:

```c
if (ibdump) {
        msleep(50);
        pr_info("CPINT[seq=%u] ...", ...);   /* SQE214 sampled here, at :1426 */
```

`SQE214` reads `CP_SQE_STAT` **50 ms after `a6xx_flush()`**. Our workload
completes in microseconds, so the sample is taken long after any work retired.
**A constant `sqe_pc` after a completed submit is what a healthy idle SQE looks
like.** "Frozen at `0x12e`" proves nothing.

This is the eighth instrument in this project that could not fail.

## Retraction 2 — `memptrs_iova` is fine

`a6xx_gpu.c:1253` claims "memptrs_iova is 0 (confirmed three boots)". Measured:

```
RINGCPU[seq=29] memptrs_iova=0x700000000  fence=28  rptr=519
```

Non-zero and valid — image 234's aperture rebase works. The comment is stale.
So `rbmemptr(ring, fence)` = `0x700000004`, a mapped address, and the
"fence timestamp goes to an unmapped 0x4" theory is **dead**.

## What the hardware actually did

```
RBBM_INT_0_STATUS  = 0x00000000
decodes: cache_flush_ts=0 hang_detect=0 ahb_err_rb=0
         | opcode=0 ucode=0 hw_fault=0 prot=0 ahb_err_cp=0 vsd=0 illegal=0
scratch2 = 0x1d = 29 = submit seqno   (hardware read)
```

Two facts, and they must be read together:

1. **`CP_SCRATCH_REG[2]` holds exactly the submit seqno**, read from hardware.
   That register is written by the driver's own preamble packet. **The CP is
   executing.**
2. **`cache_flush_ts = 0`** — the bit the tree's own comment calls "SUCCESS,
   the fence write landed" — never fires. The stream **does not retire**. And
   **no fault of any kind** is reported: no AHB error, no hang detect, no
   opcode/ucode/hw_fault/protect/vsd/illegal error.

So: the CP runs its preamble, then the submission quietly stops completing,
with no error anywhere. That silence is why ~20 images produced a black screen
and no diagnostic.

## The ring is well-formed

One submit is 15 dwords, and `ndwords=15` matches exactly. Decoded by
structure (not by guessed opcode tables — see below):

```
[519] 0x70460001  [520] 0x00000018
[521] 0x70460001  [522] 0x00000019
[523] 0x70bf8003  [524] 0x0002c000  [525] 0x00000007  [526] 0x0000015a
[527] 0x48088501  [528] 0x0000001d
[529] 0x70460004  [530] 0x80000004  [531] 0x00000004
                  [532] 0x00000007  [533] 0x0000001d
```

- `[523..526]` is a 4-dword packet carrying the 64-bit address
  `0x00000007_0002c000` = **`0x70002c000`** with size `0x15a` (346 B). This is
  the indirect-buffer reference to the user's command stream.
- `0x70002c000` is exactly the cmdstream IOVA recorded in the BOS dump
  (`mmoff=0x10217c000`, 4096 B, 103 nonzero dwords in the NDRAW=11 run). **The
  IB address is correct.**
- `[527] 0x48088501` + `[528] = 0x1d` (29) is the `CP_SCRATCH(2)` write —
  which explains `scratch2 == seqno`.
- `[529..533]` is the 5-dword `CACHE_FLUSH_TS`: event `0x80000004`
  (`CACHE_FLUSH_TS | IRQ`), timestamp address `0x00000007_00000004`
  = `0x700000004` = `memptrs_iova + offsetof(fence)`, seqno 29. Also correct.

Every field is right. The ring is a well-formed a6xx submit pointing at a
valid command stream with a valid fence address.

## Honest limit on this analysis

I decoded the ring **structurally**, using the packet shapes that are
self-evident from the payload semantics (a 64-bit address + size is an IB
reference; `0x80000004` is the documented `CACHE_FLUSH_TS|IRQ` encoding from
upstream `a6xx_submit()`). I did **not** build a full opcode table, because
`github.com/freedreno/qrisc` and `adreno-regs` both return 404 and the
Mesa-generated `a6xx_packets.h` is not present locally. Hand-guessing PM4
header layouts has produced two fabricated false claims in this project, so I
stopped rather than guess. The three claims above do not depend on the opcode
numbers, only on the payload values.

## Where that leaves the search

| Layer | Status |
|---|---|
| Command stream contents | **Correct** — state, N `CP_DRAW`, all relocations resolve |
| Ring preamble + IB pointer | **Correct** — 15 dwords, all fields verified above |
| CP execution | **Happens** — `CP_SCRATCH_REG[2]` == seqno, hardware-read |
| Ring prefetch (ROQ) | **Happens** — RPTR tracks WPTR |
| Stream retirement / fence timestamp | **FAILS** — `cache_flush_ts=0`, no fault |
| Effect on the IB's contents | **Nothing written** — RT stays all-zero |
| Render path (UBWC/tile/bin) | **Ruled out** — full matrix, all fail identically |

The single unexplained fact is now narrow: **the CP executes the preamble but
the submission never completes and never faults.** Either

- the CP silently stops at/after the `CACHE_FLUSH_TS`, discarding the IB; or
- the IB is fetched but its contents are ignored/aborted without an error.

## Next steps

1. **Time-resolve execution.** Replace the single post-hoc `msleep(50)`
   sample with a burst of samples taken between `a6xx_flush()` and completion,
   so we can watch `CP_RB_RPTR`, `CP_SCRATCH_REG(2)` and the interrupt status
   evolve and find *where* it stops. This is the measurement whose absence has
   cost this project the most.
2. **Compare `CP_PROTECT_CNTL` / `CP_RB_CNTL` against upstream.**
   `CP_RB_CNTL` reads `0x20c` with `NO_UPDATE (0x08000000)` clear; upstream
   sets `NO_UPDATE` when `!hw_apriv`. Also verify the `CP_PROTECT` span list —
   our value is `0x3` (`ACCESS_PROT_EN | ACCESS_FAULT_ON_VIOL_EN` +
   `LAST_SPAN_INF_RANGE`), and a protect violation would show as
   `prot=0x10`, which we do **not** see, so protection is probably not it.
3. Only after 1-2: revisit the RB-write oracle. It remains uninformative while
   streams do not retire, regardless of whether the SQE runs.

## Standing lesson (updated)

Eight instruments in this project signalled "the GPU ran" or "the SQE is
wedged" when they could not distinguish those states. `CP_SCRATCH_REG[2]`,
the fence, `RPTR`, `CP_EVENT_WRITE` and `sqe_pc` are all *point-in-time*
samples taken after the work completed. The only sound verdict so far is the
interrupt decode: the stream does not retire, and it does not fault.