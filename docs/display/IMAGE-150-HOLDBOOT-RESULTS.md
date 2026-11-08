# Image 150 run — silent firmware, retry works, device stable

## Run
- Attempt 0: hfi silent 5s → `-110`. `RETRY142` → reset → `-110` →
  attempt 2 → reset → `-110` → suppress. All 3 attempts clean, guard
  silent (no panic), BC132 depths 1–7, POSTINIT wrapper logged.
- HOLD150 untested (hw_init never entered; nothing to hold).
- Device alive 374s+ post-failure, healthy. No storm/faults/panics.

## Reading
- Retry + guard + breadcrumbs all proven live (not just code-reviewed).
- Inverted correlation stands out: graceful-failure boots live for many
  minutes; deaths follow SUCCESS + load. Next success run must watch
  the hold_boot question (does skipping the OOB handoff extend life?).
- Uptime/timeline discipline held via persistent-log line order
  (dmesg-only reads discarded as unreliable under drain).

Image 150 `a2ffa3e9…`. SDE 0, DT = 121, prod untouched. Stopping.
