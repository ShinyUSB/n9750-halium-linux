# Image 149 run — retry + guard proven, recovery awaits firmware

## Run (silent-firmware boot)
- Attempt 0: hfi silent → `-110`. `RETRY142 attempt 1/2` → reset →
  `-110` → `attempt 2/2` → reset → `-110` → suppress. All 3 attempts
  exhausted cleanly, device alive 214s+.
- **Guard proven:** two `reset()` executions, zero regulator panic
  (would have BUGged pre-148). **Retry machinery proven** end to end.
- Non-fatal domains: no fault this run (nothing to absorb). Untested
  live, armed.
- Second sysfs write = no-op (attach strictly once per boot; the old
  "duplicate" sightings remain unexplained-or-misattributed, still
  watched via ATT138).

## Standing
- Retry converts would-be instant failures into 3 chances; recovery
  itself needs one cooperative firmware response (statistical).
- SMMU fault path now non-fatal + retried instead of panicking.
- Faults/panics this run: 0. Storm: 0.

Image 149 `8db8d80b…`. SDE 0, DT = 121, prod untouched. Stopping.
