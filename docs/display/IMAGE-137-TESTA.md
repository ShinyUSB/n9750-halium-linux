# IMAGE-137 TEST A — BASELINE_PROVEN_PATH (vote-ACK run)

## Result
- Vote ACK (`id=30 error=0`, `hfi_ret=0`) on first attempt.
- DCVS OOB `-110`, GPU_SET OOB `-110` — both time out, both non-fatal
  (OOB132 + BC132 confirm each).
- Mode-1 SKIP → `[25] ret=0` → `POSTINIT[10] HW_INIT_RET ret=0` →
  `[30] IRQ_REENABLE` → `[40] RETURN_TO_CALLER ret=0`.
- BC132 breadcrumb ring functional (depth 1–5, ms-stamped).
- LAST_GOOD_EVENT: `RETURN_TO_CALLER ret=0` (t=30.62s).
- FIRST_BAD_EVENT: silent reboot sometime after (no panic/fault/storm
  in retained buffers).

## Classification: BASELINE_PROVEN_PATH
OOB lifetime: GPU_SET cleared at [23]/[24] (`clear state=1/0`) AFTER
hw_init as designed. No runtime-PM/devfreq callbacks fired on this path
(none exist in-tree for it — noted, not added).
