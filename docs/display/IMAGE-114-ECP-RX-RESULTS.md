# Image 114 — ECP decode: no response at all

## ECP request (exact, verified live)

q0 rd=61 wr=61 (drained), q1 rd=7 wr=8, INTR_INFO=0x0.
`feature=1 enable=0 data=0 size=16` (4 dwords).

## RX classification (A–F)

- No ACK for ECP (not A), no ERROR (not B/F), no unexpected-ID ACK (not C).
- One transient: q1 wr 8→16 with zero content (D-like, but zeros, not a
  packet; never resolved into content on later reads).
- `hfi_irq` total = 4 against 2 consumed messages (version + perf ACKs):
  2 extra fires with no corresponding message (E-like).
- No SMMU fault, no panic; device stable (gate held).

## Standing pattern

GMU answers exactly two messages (version, perf) then goes silent — BW and
ECP both unanswered, with IRQs firing sporadically and no content. This is
consistent with firmware gating further processing on state it never reaches
(post-BADDFACE verdict path), not with transport failure: transport demonstrably
delivers both directions when the firmware participates.

Image 114 `01a377e0ec80f502dd26f83611ad8397f73759cab066bc9d0cd58e090062984e`.
SDE 0, DT +0, production untouched. No fix applied.
