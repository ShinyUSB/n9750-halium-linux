# Idle-longevity watch — COMPLETE: pristine boots survive

## Result
- Pristine boot (no trigger): alive 1916s+ (32 min) and counting, healthy.
- Triggered boots die ~1–12 min after GPU activity. Failed/suppressed
  boots live long; successful+loaded boots die sooner or later.
- Conclusion: deaths REQUIRE GPU activity. Port instability exonerated.
  (Caveat: trigger writer unknown for one unattended 12-min-in run —
  4th instance of the phantom trigger; runs well-formed regardless.)

## Unattended run bonus datum
- Full chain (no-BW) → vote TIMEOUT → retry-1 → reset started. Outcome
  cut by reboot; retry second attempt unobserved. Vote tally moves.

## Standing
- Survival requires either no GPU activity (proven) or post-success
  stability we don't have yet. Next: hold_boot survival test on a
  success run (150 already built for exactly this), then Mesa.
