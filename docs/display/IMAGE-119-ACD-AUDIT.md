# Image 119 — audit STOP: no real ACD/DVM values exist in-tree

## 1. Current PERF assignment (`a6xx_hfi.c`, `a6xx_hfi_send_perf_table`)
`msg` is zero-initialized; loop sets only `.vote` and `.freq`.
`.acd` is never assigned → **acd = 0x00000000 for every GX level**.
(The prescribed edit target `msg.gx_votes[i].acd = 0xffffffff;`
does not exist in our code.)

## 2. Search for real per-level ACD/DVM platform data — all negative
- `acd_dvm` in `drivers/gpu/`, `arch/arm64/boot/dts/qcom/`: 0 hits.
- `dvm` (any case) in our `drivers/gpu/drm/msm/` + sm8150 DT: 0 hits.
- `acd` in our `drivers/gpu/drm/msm/`: only the struct field
  (`a6xx_hfi.h:104`) and one comment — no parser, no sender, no storage.
- `qcom,gpu-pwrlevel@N` nodes: only `qcom,gpu-freq`, `qcom,bus-freq/min/max`.
  No ACD property at any level.
- `a6xx_gmu_rpmh_arc_votes_init`: parses ARC votes from OPP only, no ACD.

## 3. `qcom,gpu-acd-table` (sm8150-v2.dtsi:773) is NOT a source
It carries `acd-enable-by-level/stride/num-levels/acd-data` for the
separate HFI ACD-table message (`HFI_H2F_MSG_ACD` in donor master),
not per-level `gx_votes[i].acd`. Our tree has no parser or sender for
that message. Mapping its 5 data pairs onto GX levels would invent a
mapping — prohibited.

## 4. Reference values (not adopted — reported for context)
- Samsung KGSL for this device (`kgsl_hfi.c:508`): `acd = 0xFFFFFFFF`,
  commented "Hardcode this to the max threshold since it is not used".
- Donor master (`a6xx_hfi.c:344`): also `0xffffffff`.
- Ours (`0x00000000`) matches neither reference, yet PERF still ACKs
  (Image 118: `HFI-ACK id=4`), so the zero-vs-sentinel difference does
  not block the PERF ACK itself. Recorded, not acted on.

## Decision per instruction 4
No `acd_dvm_val`-class data exists anywhere in our tree. **STOP: no source
change made, no image built.** Substituting zero (current), `0xFFFFFFFF`
(vendor sentinel, not a platform-derived value), or ACD-table data would
all violate "do not invent values."
