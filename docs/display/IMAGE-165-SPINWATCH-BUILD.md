# Image 165 (build-only) — timer-free spinwatch

## Why
Triplicated evidence that classic timers stop delivering ~50–250ms
post-`[25]` (pet_timer, dw161 delayed_work, gvitals work — all miss)
while hrtimers/scheduling continue, on CPUs proven awake (spinners).
Every timer-based witness goes blind exactly when needed. A kthread
that busy-waits on `sched_clock` (+ `cond_resched`, no timers, no
sleeps) cannot be stopped by timer failure: regs at +1s, regs +
`show_state` at +5s. The +5s dump reveals the pet kthread's exact
wait point (timer wait vs IPI ping vs MMIO) — the decisive
ping-vs-tick datum.

## Diff (on top of 164, `a6xx_gpu.c` only)
- `sw165_*` kthread, armed once per hw_init success alongside DW161.
  Bounded 5s spin, normal priority, exits on its own.
- Untouched: all 160–164 behavior.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- Binary contains `DW161-SPIN`, `SPIN5`, `sw165`.
- Packaged with 160 cmdline.
- Image 165 `165-halium-boot-a6xx-spinwatch.img`
  sha256 `5e16bfdcd3a6da92b4891c94f825630e96271696376fe8636a8897f4cf10bd1d`.

## Readout
- SPIN1/SPIN5 present → post-run state + pet-kthread wait point.
- SPIN1 regs collapsed → firmware collapse, finally caught red-handed.
- SPIN5 absent → even a running thread dies <5s (hard wedge class).

No flash performed (needs Download mode); stopping.
