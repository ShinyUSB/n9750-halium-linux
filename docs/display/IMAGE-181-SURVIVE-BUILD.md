# Image 181 — make a bring-up attempt non-fatal (survivability batch)

## Why (this is the pivot away from images 165–180)

Every death in the archive is the same event: `NON_SECURE_WDT`, always
with `msm_watchdog 17c10000.qcom,wdt: Watchdog bark! Now = 105.23 /
last pet at 94.23` — exactly the DT `qcom,bark-time = <11000>`. The
archive's own history proves the rest of the escalation surface is
already gone: the GMU `BUG()`s were replaced with recovery in 166, and
the built `.config` shows softlockup, hung-task and hardlockup
detectors all off. The only thing left that can kill the machine is the
apps watchdog itself, so for 16 images we have been debugging a GPU
problem through a fuse that converts every observation into a reboot.

Concretely, what the fuse amplifies:

- The pet path's weak link is `ping_other_cpus()` →
  `smp_call_function_single` to every online CPU. Images 173/175/176
  hardened it (async + 100ms timeout, >30s silent-skip) and it still
  barks: `DW161-IPIX: cpu=3 TIMEOUT dt_ns=1011705732`, then
  `alive_mask=f7`, then bark 11s later.
- A CPU that stops taking IPIs can also strand the RT pet kthread
  wherever the scheduler last put it.
- Runtime disarm is impossible: `echo 1 > .../17c10000.qcom,wdt/disable`
  fails with `Input/output error` /
  `Failed to deactivate secure wdog` (SCM refuses — verified live on
  the device this session). So this had to be a build change.

## Diff (batched 1–3 on top of 180)

**1a. Pet-on-bark** — `drivers/soc/qcom/watchdog_v2.c`,
`wdog_bark_handler()`: replaced `sec_debug_prepare_for_wdog_bark_reset()`
+ `msm_trigger_wdog_bite()` + `panic()` with
`__raw_writel(1, base + WDT0_RST)` + `BARK181` log. The handler runs in
IRQ context on a live CPU even when the pet kthread is wedged inside a
cross-CPU IPI, so the counter is cleared and the BITE deadline
(bark + 3s) never arrives. Dropping the sec_debug call also means the
next boot no longer inherits `upload_cause` / `set_dload_mode`, which is
what was dragging the phone into dump-mode reboots.
New field `wdog_dd->bark_count` counts events.

**1b. Never arm it** — cmdline `watchdog_v2.enable=0`. `probe()` returns
`-ENODEV` before `init_watchdog_data()`, so nothing is programmed and
no bark IRQ is registered. Belt and braces with 1a: 1a survives the
failure, 1b removes the failure.

**2. Starve the pet path of the link that wedges**
- `arch/arm64/boot/dts/qcom/sm8150.dtsi`: removed `qcom,ipi-ping` →
  `do_ipi_ping = 0` → `ping_other_cpus()` never runs. The alive mask is
  used only by the bark dump; one diagnostic line is a fair price.
- `watchdog_kthread()`: `set_cpus_allowed_ptr(current, cpumask_of(0))`
  — the RT pet thread stays on the CPU that has been up since boot.
- `pet_watchdog()`: `WDT0_RST` write is now the FIRST statement. The
  image-176 read-stability loop and the image-178 `PET178` markers are
  gone; the status read afterwards is bookkeeping only and can no longer
  delay a pet. `PET174`'s two per-cycle printks are gone too (two
  printks every 9.36s for the life of the boot, into a ramoops ring a
  dying SoC is already overflowing).
- Removed the now-dead `keep_alive_response()` (unused-function warning
  once the sync IPI path went away in 173).

**3. Remove the panic escalations** — `configs/d2q_a6xx_drm_experimental.config`:
`# CONFIG_PANIC_ON_OOPS is not set`, `# CONFIG_QCOM_FORCE_WDOG_BITE_ON_PANIC is not set`.
A `WARN_ON`/`BUG_ON` still present in the a6xx path (mmio NULL, fw_image
NULL, length checks, `BUG_ON(!node)`) now costs a log line, not the
machine. Re-harden both when bring-up is done and this tree stops being
a probe harness.

**Operational (on the device, done)** — `gmu-autotrigger.service` and
`gmu127-capture.service` disabled and stopped. The phone was in a
~2-minute reboot loop while this was written (archives 15:34:02 →
15:51:19, ten consecutive `NON_SECURE_WDT`), because the autotrigger
fired 60s into every boot. Trigger by hand over SSH from now on.

Untouched: every a6xx/GMU behaviour, the whole bring-up sequence, all
165–180 lifecycle logic, the production kernel, SDE/display/BT/WiFi.

## Cmdline budget

`cmdline148.txt` was 511 chars against the ~512B bootloader truncation
cap, so `watchdog_v2.enable=0` (25 bytes) needed reclaiming. Dropped
`msm_rtb.filter=0x237` (Samsung RTB trace-buffer filter; no consumer in
this port) → `cmdline181.txt`, 511 chars, a6xx flags still leading, new
flag directly behind them. `watchdog_v2` is a module param, so it needs
no DT or Kconfig change.

## Checks

- build: 0 errors. Only pre-existing Kconfig type-redefinition warnings
  from `drivers/samsung/debug/Kconfig`.
- `.config`: `CONFIG_PANIC_ON_OOPS_VALUE=0`, both symbols not set.
- `Image-dtb` contains `BARK181: bark #%u survived, petted from IRQ
  (RST written)`; no `PET174`/`PET176`/`PET178` left.
- built `sm8150.dtb`: `qcom,ipi-ping` absent from the wdt node.
- packaged with the same ramdisk as 180 (sha
  `eb5d2eb53d176ea1c6f72db0bc23b6727447202f23d086982928fd0f83e1be7c`),
  same board name `FRPSC27A006`, header v1, page 4096; cmdline 511 chars
  and NUL-terminated inside the 512-byte header field.

Image 181 `181-halium-boot-a6xx-survive.img`
sha256 `ec62e50d55f172124891ca6d18267d3d755a0d386cca8677def566c232bf9482`.

## Readout

- Boots, then `echo 1 > /sys/devices/platform/soc/ae00000.qcom,mdss_mdp/a6xx_gpu_load`
  → full bring-up → `SELFTEST142: done ... ALL 10 PASS` → SSH still
  alive minutes later. Killer bypassed; root cause becomes non-fatal
  background work and real power management toward Mesa is unblocked.
- `BARK181` visible but no reset → the GPU path does wedge the pet, and
  now we can watch it happen instead of rebooting through it. This is
  the expected intermediate result and is itself the finding.
- Still `NON_SECURE_WDT` with no `BARK181` → something else is arming or
  biting the WDT; check `dmesg | grep -c "MSM Watchdog Initialized"`
  (must be 0, proving `enable=0` arrived) and re-check that the cmdline
  survived truncation on the live device.
- No bark, dies anyway → WDT exonerated entirely; hunt the remaining
  reset vector with the machine no longer able to reset on its own.