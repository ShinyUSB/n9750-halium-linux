# Image 95 — sentinel mapped: DEADBEE0 fault GONE, GMU advances, new hang in hw_init

## Layout as run (verified live)

```
LAYOUT dummy=0x60000000/0x1000 hfi=0x60001000/0x4000
LAYOUT icache=0x4000/0x3c000 dcache=0x44000/0x3c000
LAYOUT sentinel=0xdeadb000/0x1000 (READ|WRITE, no PRIV)
SENTINEL mapped 0xDEADB000/0x1000 filled A5
```

## A–E interpretation

- **A) 0xDEADBEE0 fault disappeared.** No SMMU fault anywhere in the boot.
  GMU advanced: clocks → IRQ → DDR → fw_load → rpmh → CM3 writes →
  `gmu_start` (SYSRESET dance, no fault) → ~1 s poll → `ret=-110`
  (INIT_RESULT never `0xbabeface`) → resume/hfi path continues →
  `msm_gpu_hw_init` entered.
- **B) Scratch MODIFIED before start:** `PRESTART sentinel=00000000 ×4`
  (was `A5A5A5A5` at probe fill). Words [4..7] unknown (fault-path snapshot
  never ran — no fault occurred). Zeroing happened between probe fill and
  gmu_start entry, i.e. while GMU was clocked but before SYSRESET assert —
  consistent with the GMU executing from clock-enable and using
  0xDEADxxx as live scratch (PSP stack page).
- **C) No different IOVA fault.** Zero SMMU faults this boot.
- **D) FW_INIT_RESULT never reached:** `gmu_start` poll timed out (-110);
  HFI start not reached (resume returned error first).
- **E) N/A** — mapping worked (no fault to explain away).

## Next failure/state transition (captured, not fixed)

`a6xx_pm_resume returned (ret=-110)` → `enter msm_gpu_hw_init` →
`calling gpu->funcs->hw_init` → **no return marker; device later rebooted**
(hang inside `a6xx_hw_init`, post-resume). No panic/oops/SMMU record for the
reboot in the captured window.

## Standing

The poison address is functional GMU scratch, not a bug: with it mapped the
translation fault vanishes and the GMU runs. Remaining: (1) why boot never
completes (no `0xbabeface` — HFI handshake vs firmware expectations), (2) the
hw_init hang. No mappings added for old fault addresses. All other variables
unchanged (SDE 0, DT +0, production empty).
Image 95-fixed `321b49b6a263bd705ae96fcb03c55a3f3c2358a71c6c1c167d6d7d3ef9b00672`.