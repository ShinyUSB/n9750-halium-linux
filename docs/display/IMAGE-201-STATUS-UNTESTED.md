# Image 201 status — untested, not regressed

Date: 2026-10-11. Kernel `#146` (image 201), uptime 17h, no reset since flash.

## What 17 hours of capture show

`/root/cap201.log` (21 MB, attached since boot):

- `HFI-POLL` lines: **0**
- `HFI-ACK` lines: **0**
- `verify_fw_version ret=`: **never appears**
- `hw_init suppressed`: **1** (from the boot-time attempt)
- `RESUME_ENTRY`: **exactly 1**, at kernel t=53s

The single boot-time attempt:

```
[53.17] RESUME_ENTRY depth=1
[54.18] fw_start end ret=-110          <- firmware never booted (INITRES=0x0)
[56.29] hw_init suppressed after GMU boot error -110
```

The poll loop was never reached. A fresh trigger today also lost the lottery
(`BRINGUP-SUPPRESSED`) without attempting a resume.

## Verdict: 201 is UNTESTED, not regressed

Image 201 only changed `a6xx_hfi_send_msg()` — code that runs strictly *after*
`fw_start` succeeds. It cannot affect whether the firmware boots. This boot's
failure (CM3 never starts, `INITRES=0x0` throughout) is the oldest flakiness in
the project: the firmware sometimes never boots at all, independent of anything
downstream.

The decision rule from IMAGE-200-REROLLS-DEAD.md still stands and is still
unresolved:
- Poll succeeds → the IRQ-completion wait was the bug.
- Poll times out with `INTR_INFO == 0x0` → firmware genuinely never posts.

Neither branch has been taken. There is nothing to decide yet.

## What 201 needs

A boot where `fw_start` succeeds (BABEFACE), so the bring-up reaches
`verify_fw_version` and the poll loop actually runs. That is a bring-up-lottery
event, not something to engineer — keep triggering until BABEFACE appears, then
read the `HFI-POLL` line.

## Standing state (unchanged)

- Liveness gate never passed on any image. No pixels, ever.
- Static HFI programming verified with zero divergence from CAF.
- Nine images verified load-bearing, none reverted.
- `dmesg -w` capture discipline: attach BEFORE trigger, into `/root/`.