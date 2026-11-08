# Image 169 — timer-free storm timeline

## Why
Cleanest crash yet (zero tooling, 168 kernel) demolished the
timer-death theory: SHOT0/1/2 all fired on CPU0 with perfect ticks —
earlier absences were ring rotation from my own sysrq floods. New
established shape: everything timer-based AND the timer-free spin
thread go silent between +1s and +5s, while the pet (due +4.4s) also
misses and the machine barks/resets. A storm starting post-+1s that
wedges CPUs (starving timers, threads, and the pet path) fits; nothing
has ever sampled IRQ counters past +50ms.

## Diff (on top of 168)
- Spinwatch emits `DW161-SNAP` every 500ms of spin time: all three
  GPU IRQ counters + jiffies + online count, gated on sched_clock
  (no timers, no sleeps). Last snapshot before a wedge names the
  line and the time; surviving to +5s still dumps show_state.
- Untouched: everything else (stay-awake cmdline kept for the
  render window).

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- Binary contains `DW161-SNAP`.
- Packaged with 168 cmdline (slumber165=0).
- Image 169 `169-halium-boot-a6xx-stormwatch.img`
  sha256 `3fe11538d6f58f36bb96bcab5d5cb26e66f47b2e50465bcc6aaebe8b29b7bd31`.

## Readout
- A counter explodes between snapshots → storm line identified;
  fix the handler/mask/affinity, keep the machine.
- All quiet to the last snapshot → no storm; the wedge is
  elsewhere (bus fabric/MMIO stall), chase with GMU-side state.

No flash performed (needs Download mode); stopping.
