# Forensics correction (2026-10-06): ring rotation was eating evidence

## Finding
`last_kmsg` retains only ~30s / ~29k lines. Post-run spam (sysrq task
dumps ~1500–9000 lines each, five-diag exec spam from my own polling
tooling, sec_ts 110/s) rotates out everything older than the floods.
Consequence: ABSENT lines (SHOT0/1/2, [25], SLUM165, GVIT, pet) in
crash windows were read as "never ran" — they may have run and
rotated. Suspect in order: timer-death theory, slumber-wedge theory.
Surviving (late-printed, trustworthy): bark/pet timing, reset
reasons, SPIN1/SPIN5 regs, SHOTN1 (when it survives).

## Fixes deployed (operator tooling, reversible, no flash)
- Capture v11: stream `/proc/kmsg` (single reader, zero poll spam,
  nothing missed, no drain races) + 10s CPU-online witness. No
  `dmesg -c` anywhere anymore.
- Tripwire v6: quiet witness (2s cadence, D-state/irq/cpu snapshot
  only — no sysrq floods, no sync, no systemctl). Spinwatch +
  streaming capture carry the evidence now.
- Crashwatch (host): archives every boot's `last_kmsg` to
  `artifacts/gpu-bringup/crashes/` on reboot detect. No more lost
  crashes to auto-cycle speed.

## Standing (rotation-proof facts only)
- Bring-up identical every success; regs identical (GX on, quiet,
  ticks perfect at +50ms, 8 online).
- System demonstrably healthy at +5s (SPIN5 show_state completes).
- Pet stops within seconds post-run; Linux barks exactly 11.0s
  (bark_time) after last pet; TZ resets (WDT, sometimes silent).
- Slumber executes and returns (trigger back in userspace at +5s).
- Suppressed boots: 1 death in ~15 (background flakiness rate).
