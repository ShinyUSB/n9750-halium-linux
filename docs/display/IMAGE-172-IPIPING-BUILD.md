# Image 172 — all-CPU IPI ping probe (LAST instrument)

## Why
Triangulation has eliminated every pet-path link except one:
timers fire (SHOTs, even on CPU0), threads run (+5s show_state),
WDT MMIO reads fine (3us, stable), self-IPI round-trips in 1us,
storms absent, tooling/idle/hotplug-race exonerated (spinners +
single-CPU still die). Remaining: the watchdog's cross-CPU IPI
ping (`smp_call_function_single`, wait=1, every online CPU) — the
one link never tested live. A CPU that never acks wedges the pet
forever with zero other symptoms.

## Diff (on top of 171)
- Spin thread at +3s pings every OTHER online CPU the same way
  (wait=1) and times each (`DW161-IPIX: cpu=N ret=R dt_ns=T`).
  A wedge names the dead CPU by the last line standing; all-fast
  exonerates IPI delivery completely.
- Untouched: everything else.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- Binary contains `DW161-IPIX`.
- Packaged with 148 cmdline (slumber ON).
- Image 172 `172-halium-boot-a6xx-ipiping.img`
  sha256 `603b7d869d608e9ea713e55eebb25cd6c7851edff870f1ea9036bf95578f35f1`.

## Readout (each final for its branch)
- Wedge on cpu N → that CPU doesn't take IPIs post-run (fix:
  affinity/exclusion in the ping path or the CPU's underlying
  wedge — both now sharply scoped).
- All fast → pet path FULLY exonerated; the killer is not the
  watchdog path at all (back to firmware-first theories with
  recovery keeping the machine alive).

No flash performed (needs Download mode); stopping.
After this: fixes only.
