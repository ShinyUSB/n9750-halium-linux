# Image 160 (build-only) — zero GMU buffer pages

## Root-cause candidate (new)
- Both GMU buffer allocators use `alloc_page(GFP_KERNEL)` WITHOUT
  `__GFP_ZERO`. Only `log` (memset 0) and `sentinel` (A5) are initialized.
  HFI queues, icache/dcache, dummy, debug = stale RAM — including
  previous-boot GMU session residue across soft reboots.
- Uninitialized firmware-input memory explains the whole variance
  family with one mechanism: NACK/silence/BADDFACE/garbage-DMA depending
  on which stale bytes the firmware trips over. Vendor KGSL zeroes
  (their alloc path zeroes sharedmem; ours never did).

## Diff
- Both `alloc_page` loops → `GFP_KERNEL | __GFP_ZERO`.
- `reset()`: re-zero HFI queue memory after `hfi_stop` (fresh state per
  retry attempt; queues idle there, host memset safe) + `RETRY160` log.
- Untouched: everything else. Success path identical (zero pages read
  the same as clean pages would).

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- Packaged canonical flags (probe/secure/nonlegacy/selftest).
- Image 160 `160-halium-boot-a6xx-zeropages.img`
  sha256 `8e1933bef5fe186754499823b5b1cfc84041ab677d1cd4fee28c6af0833af672`.

## Readout
- Silence/NACK rate drops across boots → residue theory CONFIRMED.
- Unchanged → exonerated, look at voltage margins next.
- Watch specifically: first-try success rate + retry recovery rate.

No flash performed; stopping.
