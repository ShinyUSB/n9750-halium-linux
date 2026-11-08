# FINAL — bring-up reliably reaches HFI on 198, but the HFI exchange still times out

Captured 2026-10-10 from `/root/cap-6b92fcce.log` (775 KB), the first complete
reliable log of a boot on image 198.

## The data

```
[117.002] FORENSIC VERDICT GOOD BABEFACE ms~1     <- GMU firmware boots
[117.007] sptprac_enable -> skipping               <- image 198's guard working
[122.080] HFI: verify_fw_version ret=-110          <- HFI TIMES OUT
[124.190] hw_init suppressed after GMU boot error -110
```

## What this establishes

**Image 198's sptprac guard works and is load-bearing.** Bring-up now reliably
gets past sptprac to the HFI exchange. Before it, boots frequently never reached
HFI at all. The step is no longer the wall.

**The wall moved, and it is HFI itself.** The firmware self-initialises
correctly (`BABEFACE` = CAF's success code), then does not answer
`verify_fw_version` within the 5 s window, so `hw_init` is suppressed and the
ring is never programmed.

## Method that finally worked — and why earlier attempts failed

Sequence that produced a complete log:

1. **Attach `dmesg -w` BEFORE firing the trigger**, into `/root/`
2. Fire the trigger
3. Sleep, then run the gate

Three prior harness attempts all failed for ordering reasons:
- `measure-hfi.sh`: attached the capture at uptime ~133 s, long after the
  ~36 s bring-up. `dmesg -w` is a *streaming tail* — it reports only messages
  emitted after attach, so the whole bring-up was silently invisible.
- `run-final.sh`: fired the trigger, slept 20 s, then wrote dmesg. The reset
  landed inside the sleep, so nothing was ever written.
- `dumpd.sh`: same ordering bug.

**The rule: attach the capture first, then trigger. Never trigger-then-capture.**

`/root/` is on the same persistent `/dev/loop0` as `/var/log`; the earlier
`/var/log` files vanishing remains unexplained, but writing to `/root/` has been
reliable since.

## Confidence, stated honestly

- **Proven:** 198's guard works; bring-up reaches HFI; HFI then times out;
  `hw_init` suppressed as a consequence.
- **Not proven:** a *rate*. This is again one boot. But unlike the earlier
  n=1 claims, this log is complete and internally consistent, so the causal chain
  in it is trustworthy even if the frequency is not yet known.
- **Never established:** liveness gate passing on any image; any pixels.

## Next step

The target is the HFI exchange: firmware boots, then ignores
`verify_fw_version`. Image 195 fixed a missing `wmb()` (CAF `kgsl_hfi.c:178`)
and produced six ACKs once — so the mechanism *can* work, it is just
unreliable. That points at ordering or coherency beyond the single barrier, e.g.
cacheability of the HFI queue mapping, or a second missing barrier on the
response side.

Use the working harness pattern (`/root/`, attach-then-trigger) for any further
runs. `dmesg -w` into `/root/cap-$(boot_id).log`, then grep it after the reset.

## Standing inventory (all verified, do not revert)

| image | change | status |
|---|---|---|
| 189 | pm_runtime deadlock in msm_gpu_submit | proven |
| 190 | `MSM_VERSION_MINOR` 2→4 → `sp_funcs` | proven |
| 191 | `MSM_SUBMIT_BO_DUMP` | proven, `GEM_SUBMIT=0` |
| 192 | ITCM hole at IOVA 0 | proven, SMMU faults 5→0 |
| 193 | vblank WARN → pr_info | proven, no ~284 s reboot |
| 195 | `wmb()` before HFI doorbell | proven necessary, **not sufficient** |
| 196 | readable cpstate probe | proven, survives hard resets |
| 197 | cpstate 0640 (read+write) | proven |
| 198 | sptprac guard | proven, reaches HFI reliably |

## Retractions (kept so they are not re-derived as fact)

- Image 195 was headlined "THE GPU IS ALIVE" off one boot; it was a lucky boot.
- "3 of 4 boots never reached HFI" was not supported — that harness attached its
  capture after the bring-up had already run.
- `doorbell=0x01` in our logs is a hardcoded printf literal, not a register
  readback, and was cited as evidence for several images before this was noticed.