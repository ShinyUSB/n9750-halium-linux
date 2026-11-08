# Image 157 run — success, no mismatch this time

## Run
- Full chain (no-BW) + vote ACK with conventional `ret_id=30`
  (`hfi_ret=0`, no idmismatch line) → GBIF → cp_init → selftest 10/10 →
  `[25] ret=0` → `NON_SECURE_WDT` reset after.
- No timeouts → HFI-T untested live again. Dispatcher fix armed for the
  next 239-style answer. No storm/faults/panics.

Image 157 `2430df46…`. SDE 0, DT = 121, prod untouched. Stopping.
