# SUPERSEDED — see `STATE-STREAM-NEVER-RETIRES.md`

**The headline "the SQE never starts" is WRONG and is retracted.** `a6xx_gpu.c:1262`
does `msleep(50)` after `a6xx_flush()` before sampling `CP_SQE_STAT`, so the sample
is taken long after the work retired. A constant `sqe_pc` is what a *healthy idle*
SQE looks like. Worse, `CP_SCRATCH_REG[2]` was measured equal to the submit seqno
on a hardware read, which means **the CP does execute**.

Two sub-findings in this document remain sound and useful:
- the loaded `a630_sqe.fw` fails upstream's `a6xx_ucode_check_version()` gate
  (`buf[0]=0`, needs `>= 0x190`; `0x190` appears at dword[1] instead);
- `CP_RB_CNTL` reads `0x20c` with `NO_UPDATE` clear where upstream sets it.

The `CP_SQE_MEMBASE` hypothesis was already correctly falsified below.

---

# Original text (retained for history)

# ROOT CAUSE (NOT CONFIRMED): the SQE never starts — see retraction above

Date: 2026-11-16. Supersedes the "CP consumed the ring" claim in
`FINDING-CP-CONSUMES-RING-RB-WRITES-NOTHING.md`.

## Correction to my own previous conclusion

I previously wrote "the CP consumed the ring — CONFIRMED", from `CP_RB_RPTR`
advancing `0xcc -> 0xdb` and tracking `CP_RB_WPTR`. **That inference was wrong.**

This tree's own research says so at `a6xx_gpu.c:1406-1409`:

> "CP_RB_RPTR is advanced by the ROQ DMA prefetcher, which is a **different
> clock domain from the SQE**. RPTR==WPTR with an idle SQE is what a STOPPED
> SQE looks like, not a finished one. So we have never seen anything retire."

So RPTR advancing proves the ROQ prefetcher drained the ring into its queues.
It does **not** prove the SQE executed anything.

## The measurement that settles it

`CP_SQE_STAT` index 0 is the SQE firmware PC; index 1 is the current packet.
Three submits with **different** draw counts (3, 6, 9):

| submit | sqe_pc | sqe_cur_pkt | scratch2 | fence | RPTR/WPTR |
|---|---|---|---|---|---|
| 1 | `0x0000012e` | `0x70460004` | `0x1e` | 30 | `0x225` |
| 2 | `0x0000012e` | `0x70460004` | `0x1f` | 31 | `0x234` |
| 3 | `0x0000012e` | `0x70460004` | `0x20` | 32 | `0x243` |

**`sqe_pc` and `sqe_cur_pkt` are frozen at identical values across every
submit**, while `CP_SCRATCH_REG[2]` (the CP-domain fence), the fence counter,
and RPTR/WPTR all advance normally.

The SQE is not executing. It is sitting at one instruction and never leaving.

This explains every symptom in the project at once:

- the command stream is byte-correct (state prologue, N `CP_DRAW` packets,
  all relocations resolving) — it is simply never run;
- the clear colour is correctly emitted as `RB_BLIT_CLEAR_COLOR_DW0..3` and
  the render target is correctly relocated into the stream — never executed;
- the render target BO stays all-zero — nothing writes to DDR;
- fences "retire" — `CP_SCRATCH_REG[2]` and `RPTR` are CP/ROQ domain, not SQE
  execution, which is exactly the false proof that fooled this project.

## The RB-write oracle is moot — do not build it

I was about to hand-build an `RB_BLIT` clear to test whether the RB can write
DDR. **That work would have been wasted.** With the SQE stopped, no blit,
clear, shader or anything else in the stream can execute, so the oracle would
have returned "destination zero" for the trivial reason that nothing ran, and
that result would have been indistinguishable from a genuine RB defect.

This is a good outcome: the proposed experiment was eliminated *before* being
built, and it also honours the standing rule against hand-crafting PM4 without
reading the register header (which has already produced two fabricated false
claims here).

## Hypotheses checked and cleared

**Falsified — missing SQE context init (`CP_SQE_MEMBASE`).** `CP_SQE_MEMBASE`
is absent from this tree's register header (gap at `0x82c/0x82d`, between
`0x824` and `0x830`) and is never written. I hypothesised upstream must
program it. Checked against real upstream `a6xx_gpu.c` (codebrowser.dev):
upstream has exactly seven `CP_SQE*` references and **never writes
`CP_SQE_MEMBASE` either**. Hypothesis dropped.

**Falsified — zap firmware.** `a640_zap.{mdt,b00,b01,b02}` are dated
2026-10-14, i.e. added by a previous session, which was alarming since the DTS
does require them (`qcom,firmware-name = "a640_zap"`). But they are genuine
ARM64 ELF binaries (`7f 45 4c 46`, `e_machine=0xb7`), not stubs. Not the cause.

**Falsified — `RBBM_CLOCK_CNTL`.** An older note recorded `0x8aa8aa02` and
wondered about the missing bit 7. Upstream `a6xx_set_hwcg()` resolves it:

```c
if (adreno_is_a630(gpu))          clock_cntl_on = 0x8aa8aa02;
else if (a610 || a612)            clock_cntl_on = 0xaaa8aa82;
else if (a702)                    clock_cntl_on = 0xaaaaaa82;
else                              clock_cntl_on = 0x8aa8aa82;   /* a640 */
```

Our observed `0x8aa8aa82` is **correct for A640**. The old `0x8aa8aa02` was
the A630 value. No bug here.

**Cleared — `0xdeadbead` per-block clocks.** `CLOCK_CNTL2_RB1..3`, `SP1..3`,
`UCHE` read `0xdeadbead`. That is the standard "register not implemented on
this SKU" value; `RB0`, `SP0`, `TP0`, `RAC` all read real values.

**Correctly programmed (matches upstream).** `CP_SQE_INSTR_BASE` (upstream
line 1509), `CP_SQE_CNTL = 1` (upstream line 1547), `CP_RB_BASE`. So the basic
bring-up sequence is present.

## The strongest remaining lead

**The loaded SQE microcode fails upstream's own version gate.**

Upstream `a6xx_ucode_check_version()`:

```c
if (!strcmp(sqe_name, "a630_sqe.fw")) {
        if ((((buf[0] & 0xf) == 0xa) && (buf[2] & 0xf) >= 1) ||
            (buf[0] & 0xfff) >= 0x190) {
                a6xx_gpu->has_whereami = true;
                ret = true;
                goto out;
        }
        DRM_DEV_ERROR(..., "a630 SQE ucode is too old...");
}
```

and `a6xx_ucode_load()` does `return -EPERM` if that fails.

Measured on `/lib/firmware/qcom/a630_sqe.fw`:

```
bytes 0..15 : 00 00 00 00 90 e1 6e 01 0b 1f 00 01 00 00 00 01
buf[0]        = 0x00000000
 (buf[0]&0xfff) = 0x000   (needs >= 0x190)   -> FAIL
 (buf[0]&0xf)   = 0x0     (patched check)     -> FAIL
 whereami gate = False
 size          = 32304
```

Our tree **never calls this check** (the `SQE213` diagnostic only prints the
nibbles and carries on), so the driver proceeds with a microcode upstream
would refuse.

Notable and unresolved: the value `0x190` — exactly upstream's threshold —
does appear, but at **dword[1]** (`90 e1 6e 01` = `0x016ee190`), not dword[0].
That is consistent with a 4-byte header prefix in this CAF-era firmware, or
with a different field layout. I am **not** claiming a cause here — it is a
lead, and it must be checked before acted on.

## Also differs from upstream: `CP_RB_CNTL`

Ours reads `0x20c`. Decoding against `adreno_common.xml.h`:

| field | mask | ours |
|---|---|---|
| `BUFSZ` | `0x3f` | `0x0c` |
| `BLKSZ` | `0x3f00` | `0x02` |
| `BUF_SWAP` | `0x30000` | 0 |
| `POLL_EN` | `0x100000` | 0 |
| `NO_UPDATE` | `0x08000000` | **0** |
| `RPTR_WR_EN` | `0x80000000` | 0 |

Upstream writes `MSM_GPU_RB_CNTL_DEFAULT | AXXX_CP_RB_CNTL_NO_UPDATE` when
`!hw_apriv`, so **`NO_UPDATE` (0x08000000) is clear in our value**. A prior
session note claimed `0x20c` "matches v4.19 exactly"; that may be true for a
very old kernel but disagrees with current upstream.

## Next steps, in priority order

1. **Disassemble the SQE microcode and find out what is at PC `0x12e`.**
   `qrisc-disasm -v a630_sqe.fw` then look up `0x12e`. This says directly why
   the SQE is parked. Highest information per unit effort, needs only a
   host tool — no device risk.
2. **Establish the true firmware header layout** — is the version at dword 0
   or dword 1, and does the driver load the file verbatim? Decide whether
   `a630_sqe.fw` is the right microcode for this A640 at all.
3. **Then** decide between fixing the SQE start path in our bring-up versus
   replacing the microcode. Do not build the RB oracle until the SQE runs —
   it is uninformative while the SQE is stopped.

## Standing lesson

For the seventh time, a signal that could not fail produced a false positive:
"the ring drained, so the GPU ran." Every proof this project has ever
obtained — `CP_SCRATCH_REG[2]`, the fence, `RPTR`, `CP_EVENT_WRITE` — is
CP/ROQ domain. None of them are SQE execution. The first honest proof of SQE
execution is a changing `sqe_pc`.