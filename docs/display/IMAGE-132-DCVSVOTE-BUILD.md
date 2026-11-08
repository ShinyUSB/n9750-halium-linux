# Image 132 (build-only) — DCVS audit + gated HFI vote probe (default off)

## Source changes
- `a6xx_hfi.h`: `HFI_H2F_MSG_GX_BW_PERF_VOTE 30` + 4-dword struct.
- `a6xx_hfi.c`: `HFI_MSG_ID` table entry (designated init — no OOB);
  `a6xx_hfi_send_gx_vote()` reusing `a6xx_hfi_send_msg` (request-dword
  + header/ret logs). Declared in `a6xx_gmu.h`.
- `a6xx_gmu.c`: `gmu_hfi_probe` param (`a6xx.gmu_hfi_probe`, default 0);
  `DCVS132-A/B/C/D` audit in `set_freq` (index, PERF/BW settings,
  RETURN before/after, OOB-set ret, SPTPRAC/RPMHPWR); `RPMH132` init/
  start logs; `GX132-*` snapshots (PRE/POST/+100us/+1ms) + probe
  orchestration hooked after message-`hfi_start` success, before
  `set_freq` (vote fail → stop, like any hfi failure).
- `a6xx_gpu.c`: `GX132-CP` flag note at RINGHW site.
- `clear_oob` returns void — "OOB clear return" un-loggable (noted).
- Votes-consumed audit: `gx/cx_arc_votes` consumed ONLY by PERF sends.

## PART 7 safety gates (all pass)
- Existing queue/send reused verbatim; generic `(id,seq)` ACK dispatch
  handles id=30 with no parser change; no arch changes beyond the three
  adreno files + param.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- SDE/display 0; DT = 121 content; production untouched.
- ramdisk `eb5d2eb5…`; image 62 untouched; default cmdline has NO probe
  flag (dormant as specified).
- Image 132 `132-halium-boot-a6xx-dcvsvote.img`
  sha256 `3482c4e30442c97802a9ce7bb201696118b2195c811893d93b05a80e0727c0e3`.

Flash plan: default-off first (A/B classification via DCVS audit);
probe=1 later by cmdline repackage only (no rebuild). Stopping.
