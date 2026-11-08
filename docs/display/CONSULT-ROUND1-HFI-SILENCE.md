# Consultation round 1 — Muse Spark 1.3 + Ling 3.1 Flash (+ Nemotron pending)

Date: 2026-10-10. Kernel `#138` (image 194). Purpose: break the "firmware ignores
HFI" dead end with independent opinions, per `using-other-models.txt`.

Models consulted (IDs resolved with the `models` tool, not hardcoded):
- `opencode/muse-spark-1.3-contributor-free`
- `opencode/ling-3.1-flash-free` (adversarial framing)
- `opencode/nemotron-3-ultra-free` (launched; roster later showed this ID retired)

Note: `opencode/fledge-alpha-free` from the notes file **does not exist** in the
current roster, and `nemotron-3-ultra-free` was present at launch but absent on
re-query. Roster drift is real and fast.

## Independent convergence — the one thing both models agreed on

**The sptprac timeout is load-bearing and I have been dismissing it.**

Muse Spark: "`SPTPRAC poll TIMEOUT status=0xc0` ... is your biggest red flag. You
ignored a power/flow failure and went on to HFI. Proceeding past it invalidates
everything after."

Ling 3.1 Flash, independently: "your biggest gap ... One fault (firmware stuck
in power-up) explains sptprac timeout AND HFI silence AND no IRQs."

That convergence is the valuable signal here, not the consensus itself — it is
the one point two models reached separately, and it contradicts my framing.

## Where I was wrong or overstated

1. **`0xBABEFACE` is weaker evidence than I claimed.** Both models. It proves the
   CM3 ran *early* init only. It does not prove the HFI engine was initialised or
   that firmware reached its service loop. Ling's framing is the sharper one:
   "it *initialises, then blocks*."

2. **Ling's cheapest test, which I had not considered:** read `FW_INIT_RESULT`
   *before* releasing `CM3_SYSRESET`. If it already reads `0xBABEFACE`, then it
   is a reset default or bootloader marker, and the entire "firmware
   self-initialised" premise — which I have repeated for several images — is
   void. This is three lines and it is the highest-value next step.

3. **My "not a masking bug" claim was under-argued.** Ling: `MASK=0xff7ffffe`
   only makes sense once you know the register polarity (1=masked vs 1=enabled).
   I asserted it without decoding polarity.

## Ranked hypotheses after consultation

| # | Hypothesis | Muse Spark | Ling |
|---|---|---|---|
| 1 | Kick/doorbell never reaches the CM3 (`H2G_RAW=0x0`) | 45% | — |
| 1' | **GMU firmware wedges in the sptprac power-up step** | — | #1 |
| 2 | CM3 cannot see HFI queue memory (its own VMAP, not the APPS SMMU) | 30% | #3 |
| 3 | `BABEFACE` is stale/default; CM3 not actually running | 20% | #2 |
| 4 | HFI queue-table IOMMU mapping / cacheability wrong | — | #3 |
| 5 | Message encoding wrong | 5% | #5 |
| 6 | Interrupt mask/polarity | — | #4 |

## Test run immediately after: the GIC check

Ling named this "the single most informative check". Live result:

```
488: 0 0 0 0 0 0 0 0  PDC-GIC 336 Level  kgsl_hfi_irq
489: 1 0 0 0 0 0 0 0  PDC-GIC 337 Level  kgsl_gmu_irq
```

- **`kgsl_hfi_irq` has fired ZERO times at the GIC level.** The interrupt was
  never *delivered*, so nothing downstream can have cleared it.
- `kgsl_gmu_irq` fired exactly once.

This **corroborates** my `hfi_irqs=0(+0)` reading at an independent layer: it is
not a buggy counter or a shared-line artifact. My MASK-polarity worry
(consequence #3 above) is also moot for the HFI line — a masked-at-GIC line
would still not increment, but the point is nothing was ever delivered.

So the firmware genuinely never asserted the HFI response interrupt. The
remaining candidates are firmware-side: wedged in power-up, unable to see its
queues, or never actually running.

## Next diagnostic (not yet built)

One build carrying all three cheap discriminators:
1. `FW_INIT_RESULT` read *before* `CM3_SYSRESET=0` — is BABEFACE a default?
2. `QTBL_ADDR` + software queue header readback at send time.
3. `H2G_RAW` immediately before and after ringing the doorbell.

Result matrix:
- `H2G_RAW` never sets → doorbell/clock/power problem (hyp 1/1')
- sets, `rd` stays 0, `BABEFACE` unchanged → CM3 cannot see queues or is wedged
- `BABEFACE` already set before SYSRESET → premise void, CM3 never ran

Do **not** build further speculative fixes to the bring-up result. Three separate
speculative fixes in a row (sptprac non-fatal, vote-probe non-fatal, ITCM hole)
have each been wrong or ineffective about *why the firmware is silent*. Instrument
first.