# Image 139 (build-only) — resource path absent; WDT forensics only

## PART 1 audit: OPP resource API in 4.14
- PRESENT: `find_freq_exact`, `get_voltage`, `set_rate`, `get_opp_count`.
- ABSENT: `set_opp`, `set_bw`, `get_level`, `get_bw`, required-opps and
  opp-level parsing in `opp/of.c`.
- `set_rate` is NOT a substitute: it would apply our RPMH-level constants
  as microvolts to GDSC supplies — semantically wrong + live-clock risk.
  Rejected with rationale (not silently ignored).
- Consequence: modes 1–3 need `set_opp`; **STOPPED**. `resource_probe`
  param deliberately NOT added (a dead knob is worse than none).
- Part 4: `set_bw` absent → skipped, documented.

## PART 2 DT
- Our tables: `opp-hz` + `opp-microvolt` only. Sole `opp-level` in tree
  is my own inert 121 addition (unparsed by 4.14). No required-opps,
  no bandwidth props. Nothing consumable exists; nothing replaced.

## PART 8 ZAP: still absent
- No `a640_zap*` on device (firmware/vendor) or host stock staging
  (re-verified). DT `pil_gpu` unusable by loader (needs GPU-child +
  memory-region + MDT + hookup). No PAS implementation added.

## Diff (forensics only)
- Pre-BUG state dumps in GMU WDOG_BITE + CM3_FAULT branches (zero
  behavior change; both never fired — their firing would panic WITH
  record, which rules them out as the silent-reset cause).
- Reset-reason readout = existing TZ/sec_debug `last_kmsg` lines
  (NOC_ERROR once — self-fault; NON_SECURE_WDT post-success ×2).
- Part 5 capture: already complete (GX132/GXSTATE/RINGHW/CPIDLE).

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- SDE/display 0; DT = 121 content; production untouched.
- Packaged probe=1 + secure=1 (run-comparable).
- Image 139 `139-halium-boot-a6xx-resourcevote.img`
  sha256 `b7833fb75a0d1d8ab2594060608144389ba19a2958123d296ddb609295e66f2b`.

## Classification: F) OPP_RESOURCE_PATH_ITSELF_IS_BROKEN
Precise meaning: the set_opp/set_bw/level mechanism is ABSENT in 4.14,
not broken-at-runtime. No mode rates to report (0 runs possible);
HFI ACK rate from prior data ~mixed (firmware variance).

No flash performed; stopping.
