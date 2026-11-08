# Image 102 — CP_MSG/log implicated: GMU boots (BABEFACE), later step fails

## Layout as run (verified live)

```
LOG mapped 0x60006000/0x1000 zeroed
CP_MSG write (value=0x60006000), readback=0x60006000
HFI 4-queue table intact (size 0x36, q0-q2 on, q3 off)
```

## Outcome: interpretation A

- `gmu_start` **returned 0** — INITRES read `0xBABEFACE` in the poll loop.
  First successful GMU boot in the entire bring-up.
- ~10 ms later: `fw_start returned (ret=-110)` from one of
  `gfx_rail_on` / `sptprac_enable` / `hfi_start` (unmarked region —
  no markers exist between the two lines).
- Gate held (`hw_init suppressed … staying alive`); device alive 140 s+;
  zero SMMU faults, zero panics, zero oops.

Per the rule for A: CP_MSG/log is implicated — with the log object present
the firmware completes boot instead of reporting BADDFACE. STOP here.

## What remains open (not concluded)

Which of the three post-start steps returns -110 (all use timeout-style
polls; hfi_start's HFI_CTRL_STATUS wait is unmarked). That is the next
single question, requiring markers in exactly that 10 ms window.

Image 102 `2c37008ca2af894f24f53817ef2892c39ce20c8b65ef245c9d506b71aa1c6f81`.
SDE 0, DT +0, production untouched. No further change.