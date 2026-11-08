# Image 140 (build-only) — restore DCVS register writes in nonlegacy mode

## The one change (`a6xx_gmu.c` resume)
Nonlegacy mode no longer skips `set_freq`: the DCVS register writes
run (OOB timeout still ignored, proven harmless). Rationale: 2×2 run
matrix shows sticky ⟺ (vote ACK + DCVS writes executed); 137-att5
(vote ACK + writes skipped) was void.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- SDE/display 0; DT = 121 content; production untouched.
- Packaged probe=1 + secure=1 + nonlegacy=1.
- Image 140 `140-halium-boot-a6xx-dcvsback.img`
  sha256 `8899403a044ae2bb66bb9802bba49d9a6414fa2ef9ad82335ac59043dc92d168`.

Prediction: sticky writes + `cp_init 0` on vote-ACK runs. Stopping.
