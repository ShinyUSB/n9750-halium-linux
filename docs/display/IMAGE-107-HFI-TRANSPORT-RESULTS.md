# Image 107 — GMU answers HFI; host parser BUGs on the response

## Transport verdict: A (consumed) + response-path failure

```
A: q0 rd=0 wr=0, msg queued
B: q0 rd=0 wr=2, doorbell=0x01 sent
C+1ms:  q0 rd=2 wr=2 (GMU consumed), q1 rd=7 wr=20, INTR_INFO=0x1 (MSGQ set)
D+100ms: identical (stable)
```

The GMU read the command, wrote 13 response dwords, and raised MSGQ.
Zero SMMU faults this boot.

## New fault (host side, exact)

```
Internal error: Oops - BUG [#1], pc: a6xx_hfi_task+0x1dc via tasklet_action
-> Kernel panic: Fatal exception in interrupt -> reboot
```

`BUG_ON(HFI_HEADER_SIZE(hdr) > dwords)` in `a6xx_hfi_queue_read`
(a6xx_hfi.c:79), called from the response tasklet. The GMU's response
header declares a size larger than the response struct (19 dwords) — either
a valid big response (e.g., async boot message, not an ACK) that our
ACK-only tasklet cannot parse, or a misaligned read. q1 already stood at
rd=7 before our tasklet ran, consistent with pre-existing/async content.

## Standing

- HFI transport works both directions; GMU is alive and talkative.
- Blocker moved from firmware-boot (solved images 95–106) to host HFI
  response parsing (19-dword struct vs bigger/foreign message).
- No fix applied. Image 107
  `83aae5ab5ec8765ac4d9b8003b1acd5e80a85fb2647c4554d928b5731ad57774`.
  SDE 0, DT +0, production untouched.