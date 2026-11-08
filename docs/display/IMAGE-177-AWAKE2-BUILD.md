# Image 177 — slumber-off control (repackage, no rebuild)

## Why
First 176 run moved the wedge: ping skip engaged, yet death came
with no pet print — wedged past the ping, in/after the WDT MMIO
(the only remaining unbounded op). WDT reads were fine at +2s
(healthy variance), so the block becomes unreachable ~+13s. Prime
suspect for gating it: our own slumber path (`rpmh_stop` → PDC
sleep) — never isolated as a variable. This image changes nothing
but the cmdline to find out.

## Diff
- 176 kernel bit-identical. Cmdline `a6xx.slumber165=0` (stay awake).
- Image 177 `177-halium-boot-a6xx-awake2.img`
  sha256 `a334e4c546fbfbcc3a0d15918d23132b07008108c88cf3add8948a507628d072`.

## Readout
- Wedge returns to ping (IPIX) with MMIO clean → slumber/PDC
  implicated in the MMIO stall (fix or drop slumber).
- Same MMIO-wedge with slumber off → slumber-independent;
  hunt the WDT block's power/clock (firmware votes?).
- Survival extends → slumber was actively harmful; keep it off
  and chase renders on awake boots.

No flash performed (needs Download mode); stopping.
