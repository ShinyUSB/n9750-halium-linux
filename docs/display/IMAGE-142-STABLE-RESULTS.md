# Image 142 run — first SMMU panic since 95; 142 features untested

## Fault
- `HFI-ACK id=3` (BW, `ret_size=160`) at 63.150218 → 550µs later, during
  ECP send/wait: `arm_smmu_context_fault` → our snapshot hook (GMU
  state dumped) → `panic("No Device SMMU Fault - SID=0x405")`.
- No storm, no NACKs, no prior anomaly. Panic (with record) instead of
  silent reset — observability win, ironically.
- 142's retry/selftest/vitals never engaged (panic in IRQ context
  preempts returns). All untested still.

## 0x405 status: UNDECODED (next-image work)
- Vendor DT has only GPU streams 0x400/0x401 across 4 CBs; no 0x405
  owner anywhere in DT. Our adapter binds gfx3d_user (CB0) + gmu_kernel
  (CB5) on `qcom,qsmmu-v500`.
- Our fault hook receives iova/cbndx/sid/fsr/fsynr but prints GMU state
  instead of the fault fields — the decode data exists, just unprinted.
- Next: extend hook (instance dev, cbndx, iova, FSR/type, R/W, SID),
  verify SMR programming for our CBs, then decide fatal vs non-fatal
  for No-Device faults.

## Standing
- SMMU-fault chapter REOPENED (single sample; could be firmware
  variance like NACKs, or a real unmapped stream the BW path tickles).
- SDE 0, DT = 121, production untouched. Device alive on fresh boot.

Image 142 `a04716f1…`. Stopping.
