# KGSL-vs-DRM HFI/GMU boot-state comparison (read-only, no changes)

## §1 HFI table

| | KGSL | DRM (ours, verified live) |
|---|---|---|
| queues | 4 (CMD, MSG, DBG enabled; DSP disabled) | 2 (CMD, response) |
| header fields | version/size/offsets/num + per-queue status | same semantics |
| queue fields | status/type/size/msgs/drop/wm/req/indices | same set + iova first |
| live content | — | `00000000 00000078 00000006 0000000c` (sane) |
| fault target? | — | **never** (no fault at 0x60000000) |

## §2 Boot structures before SYSRESET

Identical: ITCM/DTCM/cache images, HFI table, no scratch descriptors on
either side. KGSL additionally has: log buffer, dummy page, DBG/DSP headers,
ACD mailbox, bus vote (vote since added; fault persisted).

## §3 Pre-start register/memory diff

All match post-images-80/86/87 (PDC, CM3_CFG, caches, QTBL/SFR/BOOT/G7,
power, RPMH). Remaining deltas: CP_MSG+log, FENCE mask variant, dummy page,
quirk parsing. None carries an address the GMU reads at fault time.

## §4 Interrupt/OOB ordering — NO DIFFERENCE FOUND

Both: clear pending → unmask needed → enable both host IRQs; no OOB
pre-start (OOB comes in gfx_rail_on, post-start); no watchdog programming on
either side. The PSP-on-exception hypothesis has **no supporting difference**
and is not asserted.

## §5 HFI start order

Both run HFI start after start→rail→sptprac. Moot for this fault: we die
inside `gmu_start`'s poll window, ~1.5 ms after SYSRESET release, before any
HFI interaction.

## §6 Trace feasibility: NOT AVAILABLE for GMU

CoreSight fabric exists in DT and drivers (`CONFIG_CORESIGHT=y`), but
`CONFIG_CORESIGHT_SOURCE_ETM4X` is **not set** and **no GMU/CM3 trace source
exists in DT**. CPU ETM cannot trace GMU firmware. Do not pursue.

## §7 Snapshot facilities

KGSL snapshots regs/TCM/HFI/log/debug; ours reproduces regs/TCM-windows/HFI
(state captured in image 90). No gap relevant to the fault.

## Answers

- KGSL initializes nothing address-bearing that DRM lacks except log/dummy
  (neither fault-adjacent).
- HFI differs (2 vs 4 queues) but is never faulted and reads sane.
- Interrupt/OOB: identical. First-ms divergence is GMU-internal execution.
- Trace: unavailable.

## Conclusion

Host-state comparison is exhausted: registers, power, clocks, HFI, interrupts
all match or are proven irrelevant, yet identical firmware faults here and not
under KGSL. The two remaining host divergences with any mechanism are the
HFI-in-dummy-slot layout and the missing dummy/log pages — both relate to
what the GMU finds (or fails to find) at 0x6000xxxx early in boot, not to
PSP directly. Next proposal (not applied): relocate HFI to ≥0x60001000 and
add the dummy page at 0x60000000 (exact v5.8 layout), keeping everything else
identical — a layout-only change targeting the firmware's memory expectations.