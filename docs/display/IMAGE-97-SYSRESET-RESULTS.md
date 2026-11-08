# Image 97 — SYSRESET assert pulse ruled out

## Sequence (single SYSRESET=0 deassert, no assert)

```
C-poll-enter: SYSRESET=0x0 INITRES=0x0 (NMI=0x1f43cf6f — live status bits,
  vary boot to boot; was 0x783cfef)
D-timeout:    SYSRESET=0x0 INITRES=0xbaddface FWBUSY=0x0 NMI=0x0
fw_start returned (ret=-110)
A6XX_DIAG: skip hw_init, GMU boot error -110; staying alive
```

## Interpretation: B

- INITRES remains `0xbaddface`, timeout remains. SYSRESET pulse ruled out.
- No SMMU fault of any kind (cf. images 86–93 faulted ~1.5 ms post-start).
- NMI cleared itself during boot in both cases; not diagnostic of the pulse.

## Standing pattern across images

| image | delta | fault |
|---|---|---|
| 86–93 | various | SMMU read fault ~1.5 ms post-start (unmapped poison/scratch) |
| 95–97 | sentinel mapped | no fault; GMU runs; boot never completes (`BADDFACE`) |

The SMMU chapter is closed: with scratch mapped the fault vanishes every
time. What remains is purely boot-completion: firmware runs but reports
failure instead of `0xBABEFACE`. No HFI/CP_MSG/log changes made.

Image 97 `cca75a5f6cf2a0b91669bb9f6a89448c121667aa77a89fe179d0c052a1510e60`.
SDE 0, DT +0, production untouched. No further patch.