# Image 147 (build-only) — fault-time SMR/S2CR table dumps

## Why
Decide (a) instance != kgsl-smmu vs (b) == kgsl-smmu with SID 0x5/CB1
by capturing the routing table at attach AND at fault, from the
driver's own software copies (zero HW-read risk, no NOC exposure).

## Diff (`arm-smmu.c`, logging only)
- Alloc path: full valid-table dump (`SMR146: table idx/sid/mask/cb/count`)
  after every SME alloc (boot/probe-time only).
- Fault path: `FAULTTBL146` full-table dump immediately before the
  existing snapshot hook (which already prints instance/cb/iova/FSR).
- Zero routing/semantic change. All 146 behavior retained.

## Checks
- full: EXIT 0, 0 errors. SDE/display 0 (verify at flash).
- Packaged with 145 cmdline (all flags + skip_bw).
- Image 147 `147-halium-boot-a6xx-smrtbl.img`
  sha256 `f7bb76b54073e43cf82c4dd48ef796ba89da83402ec3282762a12a0de03d740c`.

## Readout plan
Trigger runs: every boot logs attach-time tables (compare across boots);
on the next fault (~1/20), FAULTTBL shows exactly which entries were in
effect + instance. Then: force-fresh vs map-vs-tolerate. Stopping.
