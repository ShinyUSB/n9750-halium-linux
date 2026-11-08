# Image 175 — survive deaf CPUs (ping skip)

## Why
173/174 proved the pet wedges with the ping fix live (100ms
timeouts never engage): the wedge is in `smp_call_function_single`
itself (target queue lock held by a dead CPU), before any timeout
can trigger. Two independent callers (pet kthread, IPIX probe)
wedge on cross-CPU calls while self-IPIs, timers, threads, and
MMIO all work. Deadlocking on a dead CPU's queue lock to pet the
watchdog inverts the watchdog's purpose.

## Diff (on top of 174, watchdog only)
- Per-CPU `last_ack[]` (never zeroed, unlike ping_start/end):
  CPUs silent >30s are skipped (log once per interval). Total
  hangs still bite (kthread itself wedged → no pet → HW reset);
  partial wedges survive on live CPUs with pets flowing.
- Untouched: everything else (173's async+timeout stays as the
  first line of defense).

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- Packaged with 148 cmdline (slumber ON).
- Image 175 `175-halium-boot-a6xx-survive.img`
  sha256 `dca42a36980bc18c2585ec5a3bf7552f5bfee50382ff96ab3997915e667c4ecb`.

## Readout (the survival verdict)
- Pets flow past +11s post-run (no bark, no reset; "silent >30s"
  lines name the dead) → killer bypassed; machine lives to run
  Mesa; root cause (what kills CPUs post-run) becomes non-fatal
  background work.
- Still barks/resets → the wedge is not (only) the ping target
  set (re-examine: caller-side, or a different link entirely).

No flash performed (needs Download mode); stopping.
