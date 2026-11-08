# Image 182 — quiet bring-up (async trigger, no post-success prints, awake by default)

## Why (from the 181 runs, same flash)

The 16:47 and 16:56 archives agree on the mechanism: the sysfs writer
(`sh` pid 1241) parks in `printk` from `a6xx_pm_suspend+0x4c` →
`console_unlock` → `wake_up_klogd`, RCU stall age 6302→63017 jiffies
over t+285s→t+474s, CPU3 never runs again. SSH and OTG die on a live
phone. The machine doesn't crash; it goes deaf-mute while charger/dhd
keep logging. Secondarily, the 16:56 boot rebooted at ~t+559s through
the sec_debug dump path with an empty panic message, so at least one
panic fuse besides the (disarmed) WDT is still live.

## Diff (on top of 181)

**Async trigger** — `drivers/gpu/drm/msm/msm_drv.c`:
`a6xx_gpu_load_store` now `kthread_run`s `load_gpu_thread` ("a6xx-load")
and returns `count` immediately. A wedged bring-up costs a stuck
kthread, never the writer's CPU. (Workqueue deliberately avoided: an
earlier `schedule_delayed_work` trigger never ran on this port;
kthreads are proven by sw165.)

**Quiet suspend path** — `a6xx_gpu.c` / `a6xx_gmu.c`:
- The two `SSTEP171` prints in `a6xx_pm_suspend` are deleted
  unconditionally (the `enter` one is the proven wedger; the
  `wait_for_idle done` one goes with it). Failure path keeps a single
  `pr_warn` (rate-safe, fires once, only on failure).
- All remaining `SSTEP171`/`SLUM165`/`SLUM170` prints in
  `a6xx_gmu_stop`/`notify_slumber` gated behind new `a6xx_instr` bool
  (default off).
- Post-success instruments (`SHOTN1`, `dw161_work`, `sw165_arm` with its
  cross-CPU IPI ping + 15s hot spin) arm only when `a6xx_instr=1`.
  Default: bring-up prints `HFI`/`BC132`/`SELFTEST` (pre-success, proven
  to complete) and then silence.
- `slumber165` default flipped true→false. Rationale is twofold: the
  suspend path is the wedging call, and Mesa needs a powered GPU (no
  resume-on-submit exists yet, so slumbering before first render only
  guarantees userspace finds it asleep). Power management returns
  after first pixels. `slumber165=1` restores the old path;
  `a6xx_instr=1` restores the forensics.

**Second-layer fuses** — `configs/d2q_a6xx_drm_experimental.config`:
`# CONFIG_SOFT_WATCHDOG is not set` (bootloader forces
`soft_margin=100 soft_panic=1`; unopened today, a 100s panic fuse for
any future opener), `# CONFIG_SEC_DEBUG_FPUT_WATCHDOG is not set`
(180s fput→panic; the 16:56 reboot's window fits and its message would
not have survived the wedged console, so it cannot be exonerated from
the archive — pull it rather than argue).

Untouched: 181's bark-pet, `enable=0` posture, no-IPI pet, CPU0
pinning, no panic escalation, all bring-up/HFI/vote logic, prod
kernel, SDE/display/BT/WiFi. Cmdline unchanged from 181 (511 chars,
`watchdog_v2.enable=0` intact, a6xx flags leading).

## Checks

- build: 0 errors. Only pre-existing Kconfig type-redefinition
  warnings.
- `.config`: `SOFT_WATCHDOG`, `FPUT_WATCHDOG`, `PANIC_ON_OOPS`,
  `FORCE_WDOG_BITE_ON_PANIC` all not set; `PANIC_ON_OOPS_VALUE=0`.
- `Image-dtb` contains `handed to a6xx-load thread`, still contains
  `BARK181`, contains zero `SSTEP171: pm_suspend enter`.
- packaged with 180/181's ramdisk (sha `eb5d2eb5…`), same board/header;
  cmdline byte-identical to 181.

Image 182 `182-halium-boot-a6xx-quiet.img`
sha256 `25b934607a64b5770915496472d0d69adaf3ed829d12e1ba3301648410b9722b`.

## Readout (Mesa milestone)

- Trigger returns instantly; `SELFTEST142: done ... ALL 10 PASS`;
  SSH + OTG stay alive minutes later → the wedge class is closed and
  the machine is a usable testbed. Then: `egl-proof.py` (glClear +
  readback + fence wait) is the first render; a wedged `glFinish`
  means the CP IRQ path (never verified live) is the next fix.
- Trigger thread stuck again, but SSH alive → async containment worked
  and the remaining hang is scoped to the kthread's last line.
- sec_debug dump again with no WDT/bark/softdog/FPUT lines → the
  unidentified panicker is real and becomes the top suspect; re-enable
  `a6xx_instr=1` for one run to watch it with forensics on.

No flash performed (needs Download mode); stopping.