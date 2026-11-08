# Log channel for freezes — status and procedure

## The problem

A freeze loses everything:

1. The kernel ring buffer lives in RAM. It is gone after reboot unless copied out.
2. `/proc/last_kmsg` (Samsung sec_debug) is **overwritten by the recovery/UEFI
   boot** that follows a freeze-and-reboot. Observed three times this session
   (`Exit EBS` bootloader output instead of the frozen kernel's log).
3. `/sys/fs/pstore` is empty afterwards. **A hang writes no pstore — only a
   panic does.** ramoops is correctly configured in the cmdline
   (`ramoops.mem_address=0xc1300000`, `size=0x100000`, `console_size=0x40000`)
   but there is nothing to record for a hang.

So a freeze currently costs a reboot and yields **zero** forensic data.

## Mechanism survey (measured on device)

| mechanism | status |
|---|---|
| `systemd-run` | binary present, **unusable** — systemd is not running (`systemctl is-system-running` → `Transport endpoint is not connected`) |
| `at` | **absent** |
| `setsid` | present, `--fork` available |
| `nohup` | present |
| `dmesg -w` | **works** (verified: streams live kernel messages) |

The earlier `while true; do dmesg > f; done` logger died after exactly one
iteration because it was a plain `setsid nohup ... &` — the session leader
still belonged to the ssh pty, and tearing down that session killed the group.

## Recommended procedure

Use `setsid --fork`, which double-forks so the logger is reparented to init and
cannot be killed by the ssh session going away. Stream with `dmesg -w` rather
than re-reading the whole buffer, and write to `/root` (on persistent
`/data`), not `/tmp`.

Start it (detached, survives this ssh session exiting):

```sh
rm -f /root/dmlog.txt
setsid --fork sh -c 'exec dmesg -w > /root/dmlog.txt 2>/dev/null' \
    </dev/null >/dev/null 2>&1
```

Verify it is alive and growing — **in a second, separate ssh session**, which
is the actual test of survival:

```sh
pgrep -f 'dmesg -w'
stat -c %s /root/dmlog.txt; sleep 10; stat -c %s /root/dmlog.txt
```

It must still be running and the size must increase.

## Verification status: VERIFIED

On boot `b726d433` (image 236, kernel `#43`) the logger was started in one
ssh session, that session exited, and a **separate** later session confirmed
the process was still writing:

| | uptime | `/root/dmlog.txt` size |
|---|---|---|
| T1, session still alive | 123 s | 800,069 B |
| T2, after session exit | 465 s | **1,028,994 B** |

The starting session was gone before T2 ran, and the file kept growing. That is
the survival test, and it passes.

Practical note learned alongside it: **do not put `sleep` inside the ssh
command.** The RNDIS link drops mid-session and the remaining output is lost —
several captures returned nothing after a `sleep` that should have been fine.
Use the gap between sessions as the time delta instead.

**Start the logger first, before triggering the GPU.** It needs to be running
*before* the event it is meant to capture.

## Additional hardening worth doing

- **Make it automatic at boot**, so it is never forgotten after a flash. Without
  systemd, put it in a late init rc script or in the ramdisk init, guarded by a
  lockfile so it does not spawn twice.
- **Truncate by size**, not by rotation: a hang produces a burst, and
  `/root` is finite. `dmesg -w | head -c 8388608 > /root/dmlog.txt` keeps the
  most recent window.
- **Consider forcing a panic on fault** so ramoops actually gets written. Our
  GPU SMMU domains already set `DOMAIN_ATTR_NON_FATAL_FAULTS`
  (`msm_gpu_mmu.c:149-163`, from image 149), which converts faults into
  non-fatal errors — that choice is exactly why pstore stays empty. A
  targeted `panic_on_oops=1` or removing non-fatal handling on a debug image
  would give ramoops something to capture.
- The UFS `rpmh_write_batch` BUG **does** panic, and that class of crash *would*
  reach pstore. It has never been captured, likely because the recovery boot
  cleared the area. Checking `/sys/fs/pstore` immediately after a reboot, before
  anything else, would test that.

## Why this matters

Three freezes this session (234, 235, and the first 236 boot) produced no data.
Until this works, every hypothesis test that triggers a freeze is one-shot and
unrepeatable, and a speculative change and its evaluation get destroyed
together — the failure mode that already cost images 231 and 232.