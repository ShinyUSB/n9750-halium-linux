# Image 163 (build-only) — clock-divergence logging

## Why
Three 162 crashes show an identical signature: last pet EXACTLY 11.0s
(bark_time) before the bark, GPU run inside the gap, system otherwise
alive to the end (execs/kworkers on multiple CPUs 230ms pre-bark),
SHOT1 (+2s) missing in `last_kmsg`. Two candidate mechanisms:
(a) tick/timer infrastructure stops (jiffies-based pet_timer +
delayed_work both miss while hrtimer-based sleeps advance);
(b) pet kthread wedges in IPI-ping/MMIO while timers fine.
Spinner test (8 nice-19 loops, all CPUs awake) did NOT extend
survival — pure idle/offline exonerated.

## Diff (on top of 162, `a6xx_gpu.c` only)
- Every DW shot now logs `jiffies sched_clock ktime online_cpus`:
  jiffies-vs-sched_clock divergence at SHOT1 = tick-freeze CONFIRMED;
  lockstep = pet path specifically dead (ping/MMIO).
- Untouched: all 160/161/162 behavior.

## Checks
- fast: 0 errors, 0 warnings (after adding sched/clock.h + ktime.h).
- full: EXIT 0, 0 errors.
- Binary contains `jiffies=`, `sched_clock=`, `ktime=`, `online=`.
- Packaged with 160 cmdline.
- Image 163 `163-halium-boot-a6xx-clocks.img`
  sha256 `08737a8a2e58657c8cea445c6821eaee3310a476c595d5bbe07d73c0fe259a70`.

## Readout
- SHOT1 clocks diverged → cpuidle/broadcast/tick path (fix: idle
  constraints, broadcast audit).
- SHOT1 clocks lockstep, still dies → pet ping/MMIO (fix: watchdog
  IPI audit / bus-fabric NoC hunt).

No flash performed (needs Download mode); stopping.
