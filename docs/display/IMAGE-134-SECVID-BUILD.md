# Image 134 (build-only) — secure probe modes 0/1 (+2 stub)

## PART 1 audit (current secure path)
- Direct write `SECVID_TRUST_CNTL <- 0` in `a6xx_hw_init` after `cp_init`
  is the ONLY secure-mode handling. No a6xx ZAP hookup exists (only the
  unused a5xx `zap_shader_load_mdt/init`, which needs a `zap-shader` DT
  child + `memory-region` + `a640_zap.mdt` + SCM).
- DT HAS top-level `pil_gpu: qcom,pil-tz-generic` (pas-id 13,
  firmware `a640_zap`) — but it is NOT a GPU child and the loader cannot
  consume it. `CP_SET_SECURE_MODE` opcode defined, never used by a6xx.
- `CONFIG_QCOM_SCM=y`; `zapfw="a640_zap.mdt"` + MODULE_FIRMWARE declared.

## PART 2 firmware: MISSING
No `a640_zap*` in device `/lib/firmware`, `/vendor/firmware*`, or host
stock staging. Not extractable from available sources. No rename games.

## Diff
- `a6xx.secure_probe` param (0/1/2, default 0) + `SECVID134` snapshot
  (TRUST/TSB/base/size/STATUS/RPTR/WPTR/OOB/RPMHPWR/mode) + mode branch:
  1 skips the single write and continues; 2 logs ZAP-UNAVAILABLE and
  takes the mode-0 path (**Mode 2 NOT implemented** — missing firmware,
  DT child, and a6xx hookup; inventing PAS violates stop rules).
- Mode-1 continuation needs no flow change ([15]/[23]/[24]/[25] follow).
- Vote/GBIF/CP/HFI/OPP/DT/firmware untouched.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- SDE/display 0; DT = 121 content; production untouched.
- Packaged with 133 cmdline: vote ON (`gmu_hfi_probe=1`), secure mode 0.
- Image 134 `134-halium-boot-a6xx-secureprobe.img`
  sha256 `3a34b983787be0c3b2cdbc8a72531f60123f3e68a9b2096a1463f49d851021f3`.

No flash performed; stopping (mode 0 run first after flash).
