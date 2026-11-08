# IMAGE-132-REAL-DCVS-HFI-PROBE.md (default probe=0 run)

## PART 1 — existing DCVS audit (complete)
- `DCVS132-A: index=6 PERF_SET=0x0 BW_SET=0x0 RETURN(before)=0x0 SPTPRAC=0xc0`
- `DCVS132-B: OOB set ret=-110` → `C: cleared` →
  `D: RETURN(after)=0x0 SPTPRAC=0xc0/0x80 RPMHPWR=0x0` (two attaches:
  first SPTPRAC=0xc0, second 0x80 — OOB timeout in BOTH states).
- Caller chain: resume → `set_freq(highest)` post-hfi (unconditional,
  both attaches). `clear_oob` is void (no return to log).

## PART 2 — boot order (dmesg timestamps)
probe(50.97s, OPP/ARC/rpmh_init) → resume → `fw_start` (GOOD) →
sptprac TIMEOUT → register-hfi → message-hfi (5.2s silence, run 1) →
`set_freq`/DCVS timeout → DIAG suppress. Run 2 (101s): same to full ACKs
→ hw_init → `-22`.

## PART 5 — RPMh
- `rpmh_init`: ran once, `RPMH_CTRL=0xf11` (all vote interfaces enabled).
- `rpmh_start`: ZERO executions (FW_START state=1 path never calls it).
- No TCS activity observed. ARC votes consumed ONLY by PERF sends
  (source-verified). GX votes generated but never consumed by any RPMh
  transaction on this path.

## PART 6 — classification: A) EXISTING_DCVS_PATH_REACHES_GMU_GX_STAYS_OFF
DCVS registers address a live GMU aperture, but the firmware never acts:
OOB `-110` in both SPTPRAC states, RETURN 0, GX OFF throughout. The vote
probe (C/D/E) was NOT executed — its gate stayed off per plan.

## Collateral findings (recorded, not chased)
- Attach is REPEATABLE (two full attaches one boot, sh:1095/1366 —
  corrects prior "latched" claims; second invoker unknown).
- Version silence WITHOUT storm (zero `error=111` in entire buffer) —
  pure firmware-side variance; full success 50s later on identical bytes.
- Reboot WITHOUT storm (~105s, ~3s after `hw_init -22`): storm is not
  the reboot trigger. No panic/faults (0). Current boot alive.
- Second attach reproduced the full 128 walk (GBIF ok → CPIDLE-FAIL -22).

## Probe readiness
`a6xx.gmu_hfi_probe` ships dormant (Part 7 gates verified at build).
Enabling = cmdline repackage only, no rebuild — awaiting direction.
No source fix in this image; device stable.
