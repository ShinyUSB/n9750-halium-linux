# Image 152 run + Image 153 (100ms SPTPRAC wait)

## 152 run: SPTPRAC powers on (first time ever)
- Poll ran 11ms, timed out at 0xc0 — but status reached **0x0 (GX_ON at
  all 4 GXSTATE points)** ~26ms after poll start. Rail is slow, not dead.
- NACKs interleaved with ACKs (ids 105/34/102, sizes up to 118) — reader
  walks both. Vote ACK, selftest 10/10, `[25] ret=0`, then WDT reset.
- No faults/panics/storm-death. SPTPRAC narrative (always-failed) retired.

## 153 change
- Wait `(BIT3, 100µs delay, 100ms timeout)` so the poll reports cleanly
  instead of timing out just before the rail lands. Write unchanged.
- Packaged with 152 cmdline (probe/secure/nonlegacy/selftest, no hold).
- Image 153 `153-halium-boot-a6xx-sptprac100ms.img`
  sha256 `4268bd23591317217694bbf49d566e65bb77bca281f6c110e225ced44e88440e`.
- Build: full 0 errors; fast 0/0. SDE/DT/prod untouched (verify at flash).

No flash performed; stopping.
