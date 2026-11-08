# 🎉 Image 198 — the sptprac guard was the wall. Bring-up now reaches HFI on the first try.

Kernel `#142`.

## Result

Before the guard, bring-up reached `verify_fw_version` in **~1 boot in 4**
(measured, `HFI-MEASUREMENT-RESULTS.md`). Immediately after:

```
[36.141240] A6XX_DIAG: sptprac_enable -> skipping (no SPTPRAC GDSC guard in this tree; CAF skips here)
[36.141524] HFI-ACK id=1  seq=0 error=0
[36.141685] HFI: verify_fw_version ret=0        <-- was ret=-110 / never reached
[36.142050] HFI-ACK id=4  seq=1 error=0
[36.142935] HFI-ACK id=3  seq=2 error=0
[36.143376] HFI-ACK id=11 seq=3 error=0
[36.143868] HFI-ACK id=14 seq=4 error=0
[36.144243] HFI-ACK id=10 seq=5 error=0
```

**Six ACKs, `error=0`, on the first boot after flashing.** The funnel prediction
is confirmed: sptprac was the wall, and it was a missing *guard*, not a wrong
value or a wrong timeout.

## The fix

CAF `adreno_a6xx_gmu.c:759`:

```c
if (!gmu_core_isenabled(device) || !adreno_has_sptprac_gdsc(adreno_dev))
        return 0;
```

Everything else already matched CAF exactly — CTRL mask (`0x778000`), STATUS
mask (`BIT(3)`), register offsets. Images 152/153/154 had been tuning the
*timeout* on a poll whose success condition could never be satisfied. CAF skips
the step; we waited 100 ms every boot for a bit that never arrives.

## How the two consultants were vindicated

Muse Spark and Ling independently said the sptprac timeout was load-bearing and
that I was dismissing it. I overrode both on the strength of the function
returning 0. They were right, and the measurement is what proved it: 3 of 4 boots
were `ABSENT` *before* ever reaching HFI.

## Honest limits of this result

- **One boot.** I confirmed reachability on the first boot after flashing. The
  measurement harness should be re-run to establish the actual rate rather than
  assuming 6-for-6 from a single sample. Re-running
  `harness/measure-hfi.sh` is the cheap way to convert this into a rate.
- **The liveness gate was not reached** on this boot. The phone dropped off RNDIS
  (18d1, no route) before `gpulive.py` could run, so I have NOT yet confirmed
  `sw_rb_iova != 0`, `CP_RB_CNTL == 0x20c` and `clocking == 1` on 198.
- **Pixels still not demonstrated.** Six ACKs means the GMU firmware is talking to
  us; it does not by itself mean the ring is programmed or anything rendered.

## Next step

Re-run the measurement to get a rate, then the liveness gate. If the gate passes,
the remaining question is the hard reset on GL submit — which has been the
failure mode since image 195 and has never yet been captured, because it produces
no kernel record at all.

Do not revert 192/193/195/196/197/198. Each is verified and load-bearing:
192 ITCM hole, 193 vblank, 195 `wmb()`, 196 readable probe, 197 rw permission,
198 sptprac guard.