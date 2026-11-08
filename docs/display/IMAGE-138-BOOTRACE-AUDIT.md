# IMAGE-138-BOOT-RACE-AUDIT.md

## Classification: A) NO_TIMING_DEPENDENCY (attach→hw_init path)

## Timing runs (same image/cmdline, one point per boot)
- T+30s: ATT n=1 → providers nominal → GOOD → full ACKs → vote ACK →
  `[25] ret=0` → POSTINIT `[40]` → later `NON_SECURE_WDT` reset.
- T+80s: byte-identical outcome to T+30s (`[25] ret=0`, `[40]`, WDT reset).
- First divergence through hw_init: NONE. Attach count: 1 both
  (no duplicates reproduced). Providers/HFI/GX/CP: identical.

## Answers
- Earliest/latest failing start: no failures observed (both succeed).
- Earliest reliable success: T+30s, mid-boot (Weston/regulators active).
  "System still booting" does NOT break the path — hypothesis disproved
  for attach→hw_init.
- HFI ACK behavior / GX / CP: identical, not time-correlated. The
  ACK/NACK/silence variance floats free of boot time.
- Reboot reasons differ by cause, not by timing: NOC (138v1 self-fault),
  WDT ×2 post-success. Silent-reboot mechanism class = TZ/Watchdog,
  never Linux panic (none recorded all session).

## Notes
- Duplicate OPP warning both runs (benign, ret=0). No storm either run.
  jiffies-ms math in BOOT_RACE_READY is un-based (cosmetic; dmesg times
  authoritative).
- Part 7 delay param: declined — nothing to threshold (no timing
  dependence found). T+0/5/15 unreachable by SSH regardless.

Image 138v2 `de1a9197…`. SDE 0, DT = 121, production untouched.
