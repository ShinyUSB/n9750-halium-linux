# Image 94 — SID 0x405 audit: already routed, no patch (Phase 1 only)

## Local DT (sm8150-gpu.dtsi; v2 file has no CB overrides)

| CB node | iommus cells | fwid (`sid \| mask<<16`) |
|---|---|---|
| gfx3d_user | `0x0 0x401` | 0x04010000 |
| gfx3d_secure | `0x2 0x400` | 0x04000002 |
| gmu_user | `0x4 0x400` | 0x04000004 |
| gmu_kernel | `0x5 0x400` | **0x04000005** (matches live dump `SID[0]=0x4000005`) |

**SID `0x405` appears nowhere as an explicit entry.** The cited downstream
example (`iommus = <&kgsl_smmu 0x5>, <&kgsl_smmu 0x405>`) is a different,
single-cell binding and does not apply to this tree.

## How the driver turns those cells into hardware routing

`arm_smmu_of_xlate`: `fwid = args[0] | (args[1] << 16)` → gmu_kernel gets
SMR `{id: 0x5, mask: 0x400}`. ARM SMR match rule (`mask` bits = don't-care):
`(S ^ 0x5) & ~0x400 == 0` → matches S ∈ **{0x005, 0x405}**.

So SID 0x405 is routed by the CURRENT programmed SMRs to gmu_kernel's CB
(cbndx 1: gfx3d attached first at GPU probe, gmu second at trigger).
gfx3d_user's `{id 0x0, mask 0x401}` covers {0x0, 0x1, 0x400, 0x401} — no
conflict (verified against the driver's overlap check).

## Phase 1.6 answers (source + live fwspec, no new code)

- Owner of SID 0x405: gmu_kernel's CB (cbndx 1) via mask.
- Domain on that CB: our GMU domain (proven: one panic named
  `gmu_kernel SMMU Fault` via fwspec match; `"No Device"` on the others is
  just the name-lookup failing for 0x405, not a missing domain).
- Same domain as GMU buffers: yes.
- Translation enabled: yes (SCTLR captured `0xc000e7`; faults are FSR_TF on
  genuinely unmapped IOVAs, and mapped regions translate — cf. image 87 PF).
- SID 0x405: routed, attached, enabled. **Not missing, not misrouted.**

## Phase 2 gate: NOT met — no patch made

Adding 0x405 to the fwspec would program an extra SME `{0x405, mask 0}`
covering {0x405} on the same CB/domain — a proven no-op for both routing
(0x405 already matches) and translation (same tables). Per instructions,
no source changes, no build, no flash.

## Standing

Fault remains a translation fault at unmapped IOVAs (`0xdeadbee0` etc.)
inside `gmu_start`'s window. SDE/DT/IOMMU/PDC/parser/HFI/fences untouched.
Production untouched. No image produced (nothing changed).
SDE 0 by virtue of zero modifications.