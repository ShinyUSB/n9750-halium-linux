# HFI measurement — image 195's success was rare, and the dominant failure is EARLIER than HFI

6 boots attempted, 4 valid (1 `CAPTURE_FAILED`, 1 `UNREACHABLE` — both correctly
excluded by the harness guard rather than recorded as verdicts).

```
boots=4   OK=0   TIMEOUT=1   ABSENT=3

attempt 1: ABSENT    acks=0  hw_init_suppressed=0
attempt 2: TIMEOUT   acks=0  hw_init_suppressed=1
attempt 4: ABSENT    acks=0  hw_init_suppressed=0
attempt 5: ABSENT    acks=?  hw_init_suppressed=?
```

## What this actually says

**Zero OK in four boots.** Image 195's success — 3 HFI ACKs, `ret=0`, CP
clocking — has not reproduced. It was a real result but a rare one, and I
headlined it as a fix when it was a single lucky boot.

**The dominant failure is NOT the HFI timeout.** Three of four boots are `ABSENT`
with `acks=0` *and* `hw_init_suppressed=0`. That combination means bring-up never
reached the HFI exchange at all — there was nothing to time out. I had been
focused on HFI intermittency, and this says the real wall is one stage earlier.

Only attempt 2 reached HFI, and it timed out, latching `-110` and suppressing
`hw_init` — which is the chain already understood and already handled correctly
by image 194/195.

## Revised picture

The funnel, per boot:
1. `fw_start` + `BABEFACE` — usually succeeds
2. `a6xx_sptprac_enable()` — times out persistently (`SPTPRAC_PWR_CLK_STATUS=0xc0`)
3. `a6xx_hfi_start()` / `verify_fw_version` — reached in only ~1 of 4 boots
4. `hw_init` → ring programmed → CP clocking — only if (3) succeeds

Step 2 is where most boots stop, and it is the thing Muse Spark and Ling both
flagged as load-bearing and that I dismissed. They were right that it mattered.

## Harness notes

Two failed attempts are recorded as failures, not data:
- attempt 3 `CAPTURE_FAILED` — the guard added after the self-matching `pkill`
  bug caught it and refused to record a verdict
- attempt 6 `UNREACHABLE`

Attempt 5 has `acks=?`/`hw_init_suppressed=?`, i.e. its capture was also empty.
So the effective sample is smaller than 4: attempts 1, 2 and 4 are the only ones
with a working capture. Even so, 0 OK out of 3 fully-observed boots.

## Next step

Stop investigating the HFI exchange — it is rarely reached. The next target is
why bring-up only reaches `verify_fw_version` in about a quarter of boots, with
`SPTPRAC_PWR_CLK_STATUS=0xc0` as the strongest standing signal.