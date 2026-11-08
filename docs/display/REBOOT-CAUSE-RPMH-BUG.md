# The spontaneous reboot: RPMh timeout -> deliberate BUG() -> panic

Date: 2026-10-18
Observed on: image 270, boot `cd2f6278` (device rebooted itself, no user action)

## Verdict

**Pre-existing, and NOT caused by image 270 or by the GPU work.** The device
panicked in `rpmh_write_batch()` because the RPMh request to AOSS never
completed, and the CAF driver deliberately calls `BUG()` after 5 timeouts.

The reboot is a background hazard of this platform, unrelated to the GPU defect
under investigation.

## The evidence chain

### 1. The bootloader recorded the cause

`/proc/last_kmsg` (the UEFI/bootloader log) contains Samsung's crash-reason
extraction:

```
{738374} UploadCause[KERNEL PANIC], Don't check hangcnt
{738435} collect_rr_data : upload_cause = KERNEL PANIC ( panic_msg = Oops - BUG PC = rpmh_write_batch+0x46c/0x47c )
{738435} collect_rr_data : restart_reason = 0x776655ee
{738496} collect_rr_data : exinfo 0xc15ff000, 0x1000, 0x1000
```

`pimem_esr_sdi : [0xdeaddead]` — the secure world did not contribute a reason;
this is a kernel-side panic.

### 2. The `BUG()` is real and deliberate

`drivers/soc/qcom/rpmh.c:187-217`, `wait_for_tx_done()`, inlined into
`rpmh_write_batch()`:

```c
static inline void wait_for_tx_done(struct rpmh_client *rc,
		struct completion *compl, u32 addr, u32 data)
{
	int ret;
	int count = 4;
	int skip = 0;

	do {
		ret = wait_for_completion_timeout(compl, RPMH_TIMEOUT);
		if (ret) {
			...
			return;
		}
		if (!count) {
			if (skip++ % 100)
				continue;
			dev_err(rc->dev,
			"RPMH waiting for interrupt from AOSS\n");
			mbox_chan_debug(rc->chan);
			BUG();                    /* <-- rpmh.c:209 */
		} else {
			dev_err(rc->dev,
			"RPMH response timeout (%d) addr=0x%x,data=0x%x\n",
			count, addr, data);
			count--;
		}
	} while (true);
}
```

The `count` starts at 4 and decrements on each timeout, so the **5th**
consecutive `RPMH_TIMEOUT` reaches `BUG()`. AOSS never delivered the interrupt
for the batch, so the driver panicked on purpose.

`+0x46c/0x47c` in the panic message is the inlined `wait_for_tx_done` inside
`rpmh_write_batch` — the same PC offset seen in every archived instance of
this crash.

### 3. UFS is excluded

The known trigger for this crash in this project was the UFS clock-gating
workqueue. That has already been fixed in the DT (image 233) and the fix is
live on the device:

```
/sys/firmware/devicetree/base/soc/ufshc@1d84000/qcom,disable-lpm   -> present
/sys/bus/workqueue/devices/                                       -> no ufs workqueue
```

`qcom,disable-lpm` is read by `ufs_qcom_parse_lpm()` (ufs-qcom.c:2010) and
gates the whole `UFSHCD_CAP_*` set at ufs-qcom.c:1492, so `ufshcd_ungate_work`
is never queued. The `ufs_clk_gating_0` trigger is gone.

### 4. It is a long-standing, context-independent hazard

`artifacts/gpu-bringup/crashes/` holds **213** archived `/proc/last_kmsg`
captures. **32** contain the `rpmh_write_batch` BUG, and the triggering context
varies:

| workqueue context | count |
|---|---|
| `ufs_clk_gating_0 ufshcd_ungate_work` | 12 |
| `soc:battery sec_bat_monitor_work` | 4 |
| `events sec_ts_print_info_work` | 1 |
| (others / not attributed) | 15 |

So the same `BUG()` fires from UFS, from the battery monitor, and from the
touchscreen event work — it is "AOSS did not answer our RPMh request", not a
GPU or storage-specific fault. `qcom,disable-lpm` removed one trigger, not the
underlying hazard.

For context, the same archive contains 130 × `__call_rcu+0x2a8`, 59 ×
`drm_wait_one_vblank+0x248`, 49 × `jopp_springboard_blr_x8+0x4`, 7 ×
`lpm_cpuidle_enter+0x4f4`, and 1 × `a6xx_gmu_irq+0x1c8`.

## What we could not determine

The **exact workqueue/process** for our instance is unknown: the full kernel
panic text was not recovered. `/proc/last_kmsg` on this build is the UEFI log
and carries only the truncated `panic_msg`; `/sys/fs/pstore` is empty; there is
no `/dev/ramoops` node; and the cmdline's `ramoops.mem_address=0xc1300000` /
`ramoops.console_size=0x40000` do not yield a reachable ramoops backend on
this build (`CONFIG_PSTORE=y` but no ramoops backend registered). The dmesg ring
buffer of the crashed boot was overwritten by input-driver spam before we could
read it.

The `dev_err` lines that would have identified the context
("RPMH response timeout (%d) addr=0x%x,data=0x%x") and the requester
("RPMH waiting for interrupt from AOSS") were in that lost buffer.

## Assessment

- **Not attributable to image 270.** The patch changes only command-stream
  generation in `a6xx_submit()` plus a new MMU accessor. It issues no RPMh
  request, touches no clock or bus register, and does not run in any
  power-management path. The crash is in a subsystem the patch does not
  interact with.
- **Not attributable to the GPU work per se.** The GPU was healthy right up to
  the panic (`SPTPRAC_PWR_CLK_STATUS 0x0`, `RPMH_POWER_STATE 0x0`,
  `CP_TIMESTAMP clocking 1`).
- **A genuine hazard** for this project: it destroys GPU debugging runs at
  random, and each occurrence costs a manual reboot plus a re-established RNDIS
  link. It has cost this project at least images 228 and 232.

## Possible mitigation (not yet done)

Turn the panic into a logged error so the timeout degrades instead of killing
the system:

```c
/* rpmh.c:209 */
-		BUG();
+		/* AOSS did not answer. Do not panic the system for a
+		 * recoverable PM timeout. */
+		WARN_ON(1);
+		return -ETIMEDOUT;
```

`wait_for_tx_done()` is `void`, so it would need a return value threaded
through, or the `BUG()` simply replaced with `WARN_ON(1)` and a `return`.

This would be a **kernel change with no GPU relevance**, so it must be a
separate image from any GPU experiment — it changes the "one variable" rule if
bundled. Given that it removes a random, high-cost failure mode, it is
probably worth its own image, and it can then be carried as the baseline for
all subsequent GPU work.

Alternative, cheaper and non-invasive: keep the phone charged (it was at
14 % and the user reported the battery dying during the session) and capture
the `dev_err` context on the next occurrence by running a tight dmesg-to-file
loop across the idle window.
