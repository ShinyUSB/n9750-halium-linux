# Consultation: Muse Spark on the "core IRQ never fires" question

Asked before building image 185, per the suggestion to get a second opinion
before major changes. Muse Spark 1.3 (free tier), full brief with all
evidence, independent.

## What it got right

Its **method** was the best part, and it matched where I had landed:

- Insist on a read of the CP ring rather than a speculative fix.
- Name the exact registers to dump (`CP_RB_BASE`/`CNTL`/`RPTR`/`WPTR`,
  `RBBM_INT_0_MASK`/`STATUS`, `CP_INTERRUPT_STATUS`, `CP_SQE_CNTL`).
- Put the dump in the `a6xx-load` kthread context with power held, not in
  userspace — "must be `gpu_read()` after clocks/GDSC voted on, otherwise you
  read `0xdeadbeef`/hang."
- Correctly dismissed several non-issues, so I could stop chasing them:
  - a threaded IRQ is **not** required for delivery; plain `devm_request_irq`
    still increments `/proc/interrupts`, so a zero count proves the wire never
    asserted rather than the handler being wrong.
  - `disable_irq`/`enable_irq` around hw_init is the correct order; a level
    IRQ left pending while disabled would re-fire after enable.
  - there is no `RBBM_INT_CTL` master enable on a6xx; its absence from
    `a6xx.xml.h` is correct, not a bug. `RBBM_INT_0_MASK` suffices.
  - `RBBM_INTERFACE_HANG_INT_CNTL` only gates `HANG_DETECT`, irrelevant to a
    missing `CACHE_FLUSH_TS`.
  - `a6xx_irq()` itself is correct and matches upstream.

## What it got WRONG — and this mattered most

Its central hypothesis was **that the scratch ring left at iova 0x1000 was
being mistaken for the render ring**, so the CP would poll an empty ring
forever. It called this "the smoking gun" and the explanation for everything.

**That is falsified by logs already in our own archives.** From
`crashes/last_kmsg-1009-185607-add256f6.log` (the winning bring-up):

```
RINGHW[01] CP_RB_BASE=iova:0x1000 hw=0x1000
RINGHW: rb_iova=0x1000 rb_size=32768 sw_wptr_dw=0
```

`0x1000` is `gpu->rb_iova` — the real render ring's IOVA — not a scratch
allocation. The hardware ring base matches the driver ring exactly, and
`RPTR` advances across the selftest (`0x0 → 0x9 → … → 0x5a → 0x63`) on that
same ring. So:

- Hypothesis **(A) "CP never runs the render ring" is largely dead** — at
  bring-up time the ring is correctly programmed and the CP fetches from it.
- Its supporting observation that "no SMMU fault" implies "no fetch" is also
  wrong in the way it was used: a live, correctly-mapped ring produces no
  faults *and* no interrupts, which is consistent with what we see.

It also correctly flagged that `INT0=0x0` after a successful CP advance does
**not** prove the interrupt path is broken — the selftest's packets may simply
never have requested one. That caveat is load-bearing and I had not stated it.

## Two more useful corrections it made

- Its explanation for why the on-demand power-on matters is wrong in
  mechanism (`RB_BASE/CNTL/RPTR/WPTR` are programmed in hw_init, and ours
  demonstrably are, at `a6xx_gpu.c:~940-1005`) but right in consequence: the
  question is what the ring looks like *after Mesa submits*, which is
  unobserved.
- It correctly warned that freedreno must actually emit a `CP_EVENT_WRITE`
  with `CACHE_FLUSH_TS`, and that a packet dropped by the old kernel's
  validator would leave `WPTR` never bumping — which would look like (A)
  anyway. Worth keeping in mind as a third possibility.

## Net effect on the plan

Image 185 was already the right next move, and it is now better targeted:

1. The probe dumps the ring state *after* Mesa has submitted, which is the
   one thing never yet observed.
2. It adds the **CP perf-counter delta test** — `PERFCTR_CP_0` is clocked by
   the CP itself, so if it advances over 50 ms the CP is demonstrably running
   *right now*, independent of whether the IRQ works. That is a cleaner
   (A)/(B) discriminator than just comparing RPTR to WPTR.
3. Its dismissed non-issues are recorded so image 186 does not waste a flash
   cycle re-testing the threaded IRQ or a master enable bit.

Lesson applied: the file that told me to consult these models also says
nothing a consultant asserts is a fact until checked against the actual repo.
Worth it — but the check is not optional.

---

# Second opinion: Nemotron 3 Ultra (free tier)

Same brief, run independently so it could not see Muse Spark's answer.

## It independently converged on the same conclusion — and is also wrong

It argued (A) as well, on **different grounds**: that the normal msm ring
registers are programmed at probe time while the GPU is powered off, that
Adreno register state is lost across power collapse, and that `hw_init` never
reprograms them. Same conclusion, unrelated mechanism, same wrong answer.

It did name a decisive test, which is the useful part:

> If `CP_RB_BASE == 0` → ring was never reprogrammed after power-on. Confirmed (A).

**We already have that reading, and it is not zero.**

```
RINGHW[01] CP_RB_BASE=iova:0x1000 hw=0x1000
```

That is a *read-back* of `REG_A6XX_CP_RB_BASE` taken inside `hw_init` at
`HWINIT[21]`, and it matches `gpu->rb_iova`. The ring registers are
programmed after power-on, in `hw_init`, exactly where Nemotron says they are
missing. Its own test, applied to our data, exonerates the ring.

## Confidence signals worth recording

It never read the tree. Its register offsets are all wrong by `0x8000`:

| Nemotron said | our `a6xx.xml.h` |
|---|---|
| `CP_RB_BASE` 0x8000 | `0x00000800` |
| `CP_RB_CNTL` 0x8004 | `0x00000802` |
| `CP_RB_RPTR_ADDR` 0x8008 | `REG_A6XX_CP_RB_RPTR_ADDR_LO` 0x804 (no unsuffixed name exists) |
| `CP_RB_WPTR` 0x8014 | `0x00000807` |

It also cites `REG_A6XX_CP_RB_RPTR_ADDR` by a name this tree does not define.
So its "minimal instrumentation" snippet would not even compile here.

## What is genuinely worth keeping from it

- **Use a threaded IRQ.** Upstream msm does `devm_request_threaded_irq` with
  `IRQF_ONESHOT`; ours uses plain `devm_request_irq`. Muse Spark correctly said
  this cannot explain a *zero* interrupt count (a non-threaded handler still
  counts in `/proc/interrupts`), but Nemotron is right that it is a real
  divergence from upstream and worth fixing for correctness once the CP's
  state is known. Queued as a candidate for image 186, not as a fix for this.
- Its point that freedreno must actually emit a `CP_CACHE_FLUSH_TS` event is
  the same third possibility Muse Spark raised, and it is the one thing that
  would produce "CP ran fine, zero interrupts" with nothing else wrong.
  Worth checking in `a6xx_submit` when we have the ring dump.

## The meta-result

Two free-tier models, consulted independently, agreed — and the agreement was
worth **zero**. `using-other-models.txt` warns that on an easy task consensus
buys nothing; this is a stronger version of that. Both anchored on the same
premise ("Adreno ring programming must happen at power-on"), that premise is
plausible enough to feel like insight, and one line of our own existing log
kills it. Neither model had the logs; I did.

The consultation was still worth running, but for the opposite reason to the
one expected: it did not supply the answer, it supplied a **confident wrong
answer that was cheap to check**, and checking is what moved image 185's
design toward the CP perf-counter test instead of a ring-reprogramming "fix"
that would have cost a flash cycle and taught us nothing.