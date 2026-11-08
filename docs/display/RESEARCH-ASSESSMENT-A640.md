# Assessment of the a630/a640 research report

The report is well-sourced and mostly correct. Its **ranked list is wrong** —
rank 1 is already falsified by our own captures, and the genuinely new lead
sits in its §3.4 "divergences" section, unranked.

## Rank 1 (`RB_DBG_ECO_CNTL` = 0x04100000) — already falsified, twice over

The report's top candidate is the per-SKU blit magic in `0x8e04`, arguing a
wrong value makes the RB retire without writing. But `IMAGE-239-RESULTS.md`
already decoded it out of our own live command stream:

```
[048] = 0x408e0401  RB_DBG_ECO_CNTL <- 0x04100000 (blit magic)
[050] = 0x702c0001  CP_BLIT cnt 1
```

It is present, and it is the correct a640 value. The report's actual mechanism —
"a640 reported as a630, or vice versa" — cannot bite here either: we run Mesa
**22.3.6**, where *both* a630 and a640 use `0x04100000`. The divergence to
`0x05100000` is a later Mesa change, and this device's userspace predates it.
So there is no SKU ambiguity in this stack and no wrong value.

## Ranks 3, 4 — already satisfied or exonerated

- **R3, `UCHE_GMEM_RANGE_MIN/MAX` + 64-bit HI halves.** The report warns our
  `0x700000000` aperture rebase might have clobbered the HI halves. It did not:
  `a6xx_gpu.c` writes the GMEM window with `gpu_write64(..., MIN_LO, MIN_HI,
  0x00100000)` and `..._MAX, 0x00100000 + gmem - 1`, i.e. `[0x100000,
  0x1FFFFF]`, both halves explicit. This was on the 243 device (`ROQ243` etc.
  all logged clean).
- **R4, missing `sp_local_gpuaddr`/`sp_pvt_gpuaddr`.** The report itself notes
  mainline does not program these either — and mainline drives the same a640
  successfully on hotdog. Not required.

## The actual lead: our hwcg table is an a630 table

§3.4 flags it in passing. This is the strongest thing in the report.

Measured in our tree just now:

```
our a6xx_hwcg table: 105 entries
  SP0..SP3, RB0..RB3, CCU0..CCU3      <- four of each
  REG_A6XX_RBBM_CLOCK_HYST_TEX_FCHE: 0 occurrences
```

The report's machine diff says CAF's `a640_hwcg_regs` and mainline's `a640_hwcg`
are both **49 entries**, structured `SP0 / RB0 / CCU0` only, and differ from
each other in exactly one value: `RBBM_CLOCK_HYST_TEX_FCHE`, CAF `0x777` vs
mainline `0x0`.

So CAF and mainline agree on a640's table *structurally*, and we have a
completely different one — the **a630** table, with three shadow processor
banks, four rasterizer banks and four CCUs that a640 does not have, while the
one block a640 uniquely has (the texture fetch engine) is never programmed at
all, because `RBBM_CLOCK_HYST_TEX_FCHE` does not appear anywhere in our table.

This fits the symptom better than anything else in the report. An unclocked
texture-fetch path explains exactly what we see: the CP executes, the RB/UCHE/
CCU report busy and retire, no fault is raised, and no bytes reach DDR — because
the engine that moves color data never clocks. It also explains why every
register-tweak so far has failed: they all act downstream of a block that is
not running.

Note this is driver-side only. **No DT change, no reserved memory, no zap.**

## Correction to image 243: `GBIF_QSB_SIDE0..3` should stay ON

The report says CAF applies `GBIF_QSB_SIDE0..3 = 0x71620` for **a640**
(`a640_gbif[]`, via `adreno_vbif_start()`), and mainline never writes them for
a640. In image 243 I removed those writes on the strength of mainline's
`adreno_is_a610_family()` scoping, and defaulted `a640_qsb` off.

Given CAF is the reference implementation that actually runs on Samsung a640
silicon, I over-weighted mainline there. The default should go back on. This is
unlikely to be the root cause — 242 had QSB on and was zero, 243-v2 had it off
and was also zero — but it is the CAF-correct sequence and it costs nothing.

## What remains live, ranked

1. **Use CAF's `a640_hwcg_regs` table (49 entries) instead of the a630 table.**
   Driver-side, single change, best explanation on the board.
2. **Restore `GBIF_QSB_SIDE0..3` by default.**
3. `RB_CCU_CNTL` without a preceding WFI (Mesa-side). Only relevant on the GMEM
   path; our decoded stream was a linear 2D blit with `CP_WAIT_FOR_IDDLE`
   before `CP_BLIT`, so this looks already satisfied. Worth confirming against
   Mesa 22.3.6's `fd6_blitter.c` before spending anything on it.
4. Zap / secure mode — unchanged, still unstarted, still carrying the caveat
   that the fence echo works so a blanket secure-mode stall does not explain
   the CP-writes/RB-doesn't split.

## Where the zap work now sits

Not wasted, just no longer first. The firmware is extracted and validated
(`GPU-ZAP-FIRMWARE.md`), and the remaining zap work is a boot-critical DT edit
plus a driver port. That is a worse risk trade than a register table swap in a
file that already builds, so it should come after items 1 and 2.

## Note on report reliability

Same caution as the first research agent: several claims here are wrong or
irrelevant (R1, R4, and `a6xx_adjust_ubwc_bank_alignment()` "existing" — it
does not exist in mainline or CAF, though that is correctly flagged). The
value was in §6's structural comparison, not in the ranked conclusions. Worth
keeping, worth re-verifying before acting on any single line of it.