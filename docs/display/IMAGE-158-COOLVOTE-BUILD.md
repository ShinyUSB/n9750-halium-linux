# Image 158 (build-only) — cool-vote keepalive (power/heat test)

## Why
- Real TCS bus votes BLOCKED: no `drivers/bus/msm_bus` in tree (only
  RPMh primitives); porting the bus stack is weeks. Documented, parked.
- Remaining cheap power test: sustained 675MHz clocks. Keepalive now
  votes LOW (index 1, 257MHz) instead of max. Initial vote stays at 6
  (proven GX-on path). Contact-keeping unchanged.

## Diff (`a6xx_gmu.c`, one constant)
- Keepalive re-vote `freq`: `nr-1` → `1`. Everything else identical.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- Packaged with keepalive cmdline (probe/secure/nonlegacy/selftest).
- Image 158 `158-halium-boot-a6xx-coolvote.img`
  sha256 `6aa1373f015b723e206a0f84b544dad606430b43e39556eb49117d8c71fb8d9b`.

## Readout
- Survival >>12 min with downclock ACKs → power/heat implicated.
- Death on schedule → exonerated; next: slumber-handshake/firmware-WDT.

No flash performed; stopping.
