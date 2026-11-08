# IMAGE-135-SECVID-SKIP.md — attempt 1 SUCCEEDED (mode 0); mode 1 blocked by param bug

## Attempt log
- Attempt 1 (only attempt needed): cold boot 135 → trigger t=44s →
  full ACK chain (1/4/3/11/14/10) → **vote ACK (id=30, error=0)** →
  hw_init → GBIF ok → **cp_init ret=0** → SECVID134 reached. No retry needed.

## Mode-0 outcome: A) DIRECT_SECVID_WRITE_CONFIRMED_STALL
- Snapshot: `TRUST_CNTL=0x1 TSB=0/0/0 STATUS=0x1` (+RPTR/WPTR/OOB/RPMH
  in raw log). GPU boots SECURE ASSERTED.
- `mode=0 before direct write` printed; `after` line NEVER printed.
  Synchronous MMIO stall on the write (reads of the same register work).
- Post-reboot run only; faults/panics/storms: 0 in retained buffer.

## Mode 1: NOT TESTED (packaging bug, owned)
- `module_param(secure_probe)` sits at line 1371, BEFORE the
  `MODULE_PARAM_PREFIX "a6xx."` block (line 2391); it registered under
  the composite default name, so `a6xx.secure_probe=1` was ignored and
  mode stayed 0. (`gmu_hfi_probe` at 2403 parses correctly — vote ran.)
- Fix = move the declaration under the prefix block (one-spot source
  change) → requires a new image; not done inside 135 per no-source rule.

## Marker-name note
- 134 binary prints `SECVID134: ...` (not the literal
  `SECVID135-SKIP-BEFORE/AFTER` strings); no-source rule forbade adding
  them. Mapping: snapshot line = BEFORE, skip/write-follow lines = AFTER,
  `[15]/[23]/[24]/[25]` = next-10-operations coverage. Sufficient.

## Next
New image with fixed param placement, then mode-1 skip run. No other
change needed. Stopping.
