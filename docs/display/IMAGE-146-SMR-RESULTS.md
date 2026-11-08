# Image 146 run — stale-SMR theory DEAD; no-BW path fully works

## SMR verdict (decisive)
- kgsl-smmu handoffs: only {0x6a0,0x6c0→cb0}, {0x300→cb1}, {0x800/0x420→cb2}
  (other instances' traffic, none GPU).
- Our CBs: gfx3d_user {0x0/0x401} idx=2 **fresh**, gmu_kernel {0x5/0x400}
  idx=3 **fresh**. No reuse, no override. Stale-SMR-reuse theory dead.

## No-BW run (complete success)
- Version/PERF ACK → BW145 skipped → ECP/core/start/vote ACK →
  GBIF → cp_init 0 → **SELFTEST 10/10** → `[25] ret=0`.
- No faults, no storm, no panics. Silent reboot after (reason unretained
  this run).

## CB1 mystery: parked, hook armed
- With fresh SMRs, 0x5 must hit CB5 — yet faults recorded cb=1. Either
  the faulting instance is NOT kgsl-smmu (143 `smmu=` print will say on
  next occurrence) or S2CRs change post-attach. Fault rate 2/45: stop
  chasing actively; the armed hook catches the next one with full decode.

Image 146 `55c5a24c…`. SDE 0, DT = 121, prod untouched. Stopping.
