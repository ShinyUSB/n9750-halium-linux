# Image 173 — deaf-CPU map + ping hardening (survival fix attempt)

## Why
172's blocking per-CPU ping stopped at varying CPUs (after cpu=1,
before cpu=0 — never completing) while timers/threads/MMIO/IPI all
live-probe OK: the watchdog's wait-forever IPI ping is the one link
that can wedge the pet with zero other symptoms. Mapping alone
doesn't save the machine, so this image also fixes it.

## Diff (on top of 172)
- IPIX loop: `smp_call_function_single(..., wait=0)` + completion +
  1s `wait_for_completion_timeout` per CPU. Prints ACK/TIMEOUT each.
- Watchdog `ping_other_cpus`: same async+100ms-timeout pattern;
  unacked CPUs are skipped (stay out of the alive mask) instead of
  wedging the pet forever. Pet (RST write) proceeds regardless.
- Untouched: everything else.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- Binary contains `DW161-IPIX`.
- Packaged with 148 cmdline (slumber ON).
- Image 173 `173-halium-boot-a6xx-deafmap.img`
  sha256 `7f771b2bc8247ef781851d7de2576be3da90a8a508f798dea0456cd50fc06c2e`.

## Readout (each final)
- Survival past 11s post-run (no bark, pets flowing with "cpu N no
  IPI ack" lines) → ping wedge CONFIRMED as the killer and fixed;
  deaf CPUs get scoped next (why they go deaf post-run).
- TIMEOUT map + death anyway → ping exonerated WITH the wait
  removed too; the killer is elsewhere entirely.
- All ACK + survival → healthy path confirmed end to end.

No flash performed (needs Download mode); stopping.
