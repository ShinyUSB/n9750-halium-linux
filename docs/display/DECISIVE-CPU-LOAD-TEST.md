# DECISIVE: sustained CPU load does NOT kill it. The reset is GPU-GL specific.

Date: 2026-11-11. Kernel `#147` (image 202), phone up 17+ min at time of writing.

## The experiment

Eight pure-CPU spinners (`while :; do :; done`), nothing to do with the GPU:

```
COMMAND         %CPU
sh              99.7
sh              99.7
sh              99.7
sh              99.7
sh              99.6
sh              99.5
sh              99.2
load average: 55.81  45.00  23.52   (peaked ~56, held for minutes)
```

Result: **the phone did not reset.** Uptime kept climbing (15 -> 17 min), the
GPU stayed loaded and clocking throughout:

```
CP_TIMESTAMP ... delta 676062 clocking 1
```

Stall count did climb (9 -> 19) — so sustained load *does* aggravate the
`pm_runtime_work` stall — but the stall is harmless under load. The system stayed
fully responsive and alive.

## What this rules out

- **Not any-sustained-load.** Load average 56 for minutes, no reset. If the killer
  were load/timing-sensitive in general, this would have found it.
- **Not the `pm_runtime_work` stall.** It fires *more* under this load (19 vs 9)
  and the phone is fine. That closes it as a cause; it is a latency artifact.
- **Not the apps watchdog** (already shown disabled: `watchdog_v2.enable=0`, no
  module, no pet thread ever, zero bark lines in any log).
- **Not CPU contention / IPI starvation** in general — the classic images 173-181
  theory. Eight CPUs pegged and nothing dies.

## Therefore the reset is specific to the GPU + GL path

By elimination the remaining candidate is `glReadPixels` itself, which is the only
thing that has ever reproducibly preceded a reset — and it did so while the
system was otherwise idle and healthy.

That is consistent with everything:

| test | load | GPU | GL | outcome |
|---|---|---|---|---|
| CPU burn | avg 56 | up | none | **alive** |
| idle + GPU | ~5 | up | none | **alive** (stalls only) |
| `glstage` 3-5 | ~5 | up | clear/flush/finish | **alive** |
| `glstage` 6 | ~5 | up | **+ readPixels** | **reset** |

Stages 3-5 pass because they only *queue* work. `glReadPixels` is the first call
that requires an actual GPU->CPU round trip: it must wait for the CP to retire
the commands, which means a fence wait on a path that has never once completed
on this hardware.

So the leading hypothesis is now narrow and specific:

> **The fence/retire path never completes.** `glReadPixels` blocks waiting for a
> fence that the A640 never signals, the wait eventually times out or wedges, and
> the system dies. This is consistent with every earlier observation too —
> `wptr_dw=99` (nothing Mesa submitted), `RB_RPTR == RB_WPTR` (nothing retired),
> and `CPDUMP ... irqs=0` (**the GPU core IRQ has never fired once**, on any
> image, in this entire project).

That last point is the one I should have weighted far more heavily all along:
`linux_irq=487 irqs=0` appears in the successful bring-up logs too. The ring is
programmed and the CP clocks, but **no interrupt has ever been delivered**. Every
stage that doesn't need an IRQ works. The first stage that does need one is the
first stage that dies.

## Next step — one specific thing

Find why the GPU core IRQ never fires. Concretely:

1. Read `/proc/interrupts` for the A640 line *before* and *after* a GL submit.
   If the count is 0 in both, the IRQ is not being delivered at all.
2. Check `INT_0_MASK` against `INT_0_STATUS` in the bring-up dump:
   `INT_0_STATUS=0x0  INT_0_MASK=0x3d0e242`. If the mask is left in a state that
   blocks the ring/fence interrupt, that is the bug.
3. Compare with CAF: upstream `a6xx_gpu_init()` enables specific CP interrupts
   (`REG_A6XX_CP_INT_CP_IB_INT_STATUS` etc). Check whether this port ever writes
   them.

This is a real, bounded question with a clear next experiment, and it is the
last thing between us and pixels.

## Correction to the record

`ROOT-CAUSE-GL-READBACK-PM-LIVELOCK.md` blamed `pm_runtime_work` livelocking.
That is wrong — the system stays responsive under load average 56 and the stall
count rises without consequence. `ROOT-CAUSE-PM-STALL-IS-WDT-IPI.md` blamed the
apps watchdog; also wrong, it is disabled and never ran. Both are superseded by
`ROOT-CAUSE-RESET-STILL-UNKNOWN.md`. Neither should be re-derived.
