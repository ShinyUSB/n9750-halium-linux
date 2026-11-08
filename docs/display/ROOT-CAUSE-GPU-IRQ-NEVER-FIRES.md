# GPU core IRQ has never fired — mask is correct, so the fault is elsewhere in the IRQ path

Date: 2026-11-11. Kernel `#147` (image 202).

## The measurement

```
$ grep -iE "A640|gpu" /proc/interrupts
487:  0  0  0  0  0  0  0  0   PDC-GIC 332 Level   A640
```

**All eight CPU columns are zero.** The GPU core interrupt has never been
delivered, on any image, in the entire project. The same `irqs=0` appears in the
*successful* bring-up logs:

```
CPDUMP[auto-rest] irq: INT_0_STATUS=0x0 INT_0_MASK=0x3d0e242 CP_INTERRUPT_STATUS=0x0
CPDUMP[auto-rest] st : RBBM_STATUS=0x1 RBBM_STATUS3=0x0 linux_irq=487 irqs=0
```

This reframes everything. The GPU is powered, the ring is programmed, the CP
drains commands — and **nothing ever signals completion.** Every GL stage that
only queues work passes. `glReadPixels` is the first call that needs an actual
GPU→CPU round trip, i.e. the first that needs an IRQ, and it is the first that
dies.

## What I checked and ruled out

**The interrupt mask is correct.** I suspected `RBBM_INT_0_MASK` was never
programmed or was wrong. It is neither:

- Our `a6xx_hw_init()` does write it — `adreno/a6xx_gpu.c:959`:
  ```c
  pr_info("HWINIT[17] before interrupt-mask programming\n");
  gpu_write(gpu, REG_A6XX_RBBM_INT_0_MASK, A6XX_INT_MASK);
  pr_info("HWINIT[18] after interrupt-mask programming\n");
  ```
- Our `A6XX_INT_MASK` (`a6xx_gpu.c:698`) is **byte-identical** to upstream
  `donor/master/adreno/a6xx_gpu.c:1143`.
- Computing it from the register header:
  ```
  CP_AHB_ERROR 0x2, ATB_ASYNCFIFO_OVERFLOW 0x40, CP_HW_ERROR 0x200,
  CP_IB2 0x2000, CP_IB1 0x4000, CP_RB 0x8000, CP_CACHE_FLUSH_TS 0x100000,
  ATB_BUS_OVERFLOW 0x400000, HANG_DETECT 0x800000,
  UCHE_OOB_ACCESS 0x1000000, UCHE_TRAP_INTR 0x2000000
  = 0x03d0e242
  ```
- Live readback: `INT_0_MASK = 0x03d0e242`. **Exact match.**

So the mask programming is right, and the driver is in the same state upstream
would be. The IRQ simply never arrives.

**The IRQ is requested.** `msm_gpu.c:759` does
`devm_request_irq(pdev->dev, gpu->irq, irq_handler, IRQF_TRIGGER_HIGH, ...)`, so
the handler is registered at probe. The line exists in `/proc/interrupts`, unmasked
at the GIC level, and simply never fires.

## Where the remaining candidates are

Ordered by how well they fit "handler registered, line unmasked, never fires":

1. **The GPU never actually raises the interrupt.** `INT_0_STATUS=0x0` says
   nothing is pending. If the CP is not signalling completion because it was
   never *told* to, this is downstream of ring programming, not the IRQ path.
   Worth reading `RBBM_STATUS`/`RBBM_STATUS3` more carefully — `RBBM_STATUS3=0x0`
   is suspicious and may indicate the memory/UCACHE interface is not up.
2. **The IRQ fires but is not delivered to this CPU** — e.g. a GIC/affinity
   problem on PDC-GIC 332. The GIC-level check would show it; `/proc/interrupts`
   shows the same counter, so this is less likely but not excluded.
3. **Something de-asserts the interrupt before it is taken.** With
   `CP_INTERRUPT_STATUS=0x0` there is nothing to de-assert, which argues against
   this.

## Next step

The single most informative cheap test: read `RBBM_STATUS3` and
`REG_A6XX_RBBM_STATUS` around a submit, and compare `RB_RPTR`/`RB_WPTR` before
and after `glClear`. If the pointers do **not** advance, the CP never executed
and no interrupt would be expected — which would move the fault from "IRQ
delivery" back to "command execution", and that is a different and very tractable
problem.

Note the bring-up logs show `RB_RPTR == RB_WPTR == 0x63` tracking the software
pointer during bring-up, which suggests the CP *did* execute the boot-time
command stream. So the more likely story is that boot-time commands execute but
Mesa-submitted ones do not.

## Do not

- Do not revert image 202's coherency fix. It is proven 3-for-3.
- Do not re-derive the `pm_runtime_work` stall as a cause. It fires under load
  average 56 and the phone is fine (`DECISIVE-CPU-LOAD-TEST.md`).
- Do not re-derive the apps-watchdog theory. `watchdog_v2.enable=0`, no module,
  no pet thread, zero bark lines ever.
