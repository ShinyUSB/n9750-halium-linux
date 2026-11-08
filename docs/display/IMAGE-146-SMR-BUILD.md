# Image 146 (build-only) — SMR routing visibility

## Why
Faults #1/#2 (CB1, streams 0x405/0x5) fit stale-bootloader-SMR reuse:
`find_sme` reuses a covering entry instead of overriding, so our CB5
request could share an index whose S2CR still points at CB1. Normal
traffic (0x400) works via a different entry.

## Diff (`arm-smmu.c`, logging only)
- Handoff import: `SMR146: handoff idx/sid/mask/cb` per pre-existing entry.
- Alloc: `SMR146: alloc dev/sid/mask/idx/cb fresh-or-REUSED` per CB attach.
- Zero routing/semantic change. All 145 behavior retained.

## Checks
- full: EXIT 0, 0 errors. SDE/display 0 (verify at flash).
- Packaged with 145 cmdline (all flags + skip_bw).
- Image 146 `146-halium-boot-a6xx-smr146.img`
  sha256 `55c5a24cdae19fdf326974262d524f02a677565f9684c3437edc2967764c0fbe`.

## Readout plan
Trigger → read SMR146 lines: pre-existing entries (any →CB1?) +
our CB0/CB5 allocs (fresh or REUSED, which idx/cb). Then decide:
force-fresh vs map-vs-tolerate. Stopping.
