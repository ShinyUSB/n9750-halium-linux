# Root cause: the pm_runtime stall is the apps-watchdog IPI deadlock (images 173-181), not new

Date: 2026-11-11. Kernel `#147` (image 202). This **supersedes** the
"glReadPixels triggers a livelock" framing in
`ROOT-CAUSE-GL-READBACK-PM-LIVELOCK.md`.

## What the new evidence changes

I had concluded `glReadPixels` triggers a `pm_runtime_work` livelock. Two
measurements taken on the live device disprove the "readback triggers it" half:

| boot | GPU loaded? | GL work? | duration | `pm_runtime_work` stalls |
|---|---|---|---|---|
| `0403c0c4` | **no** | no | 982 s (full) | **0** |
| `ffbb460e` | yes | no | ~990 s | 23 |
| current | yes | no | 300+ s | 9 |

The current boot has the GPU up and clocking and has done **zero GL work**, yet
stalls every ~63 s. So:

- The GPU bring-up is what starts it (0 stalls without it, many with it).
- GL — including `glReadPixels` — is **not** the trigger. Readback is only what
  adds enough load to tip an already-stalled system into a hard reset.

That is why `glstage.py` stages 3-5 all passed and only stage 6 died: the system
was livelocked the whole time.

## The stall is a known, already-diagnosed problem

The signature matches exactly what images 173-181 chased and what
`a6xx_gpu.c:1528-1534` documents in-tree:

> Images 179-181 proved the FIRST of these two printks wedges the caller inside
> `console_unlock` (CPU3, RCU stalls 6302->63017 jiffies): the writer never
> reaches `wait_for_idle` at all.

`6302 jiffies` is the *first stall's* `t=` value in every log I have seen —
t=6303 here, t=6302 in the death log. Identical.

From `IMAGE-181-SURVIVE-BUILD.md`:

- Every death in the archive is `NON_SECURE_WDT`, always with
  `msm_watchdog 17c10000.qcom,wdt: Watchdog bark!`
- The pet path's weak link is `ping_other_cpus()` →
  `smp_call_function_single` to every online CPU
- Images 173/175/176 hardened it (async + 100 ms timeout) and it **still**
  barks: `DW161-IPIX: cpu=3 TIMEOUT dt_ns=1011705732`, then `alive_mask=f7`
- Runtime disarm is impossible — SCM refuses
  (`Failed to deactivate secure wdog`, verified live), so it had to be a build
  change

So the real chain is:

```
GPU bring-up keeps a CPU busy / interrupts its IPI servicing
  -> a6xx PM path spins (prints inside console_unlock were one trigger, removed in 181)
  -> ping_other_cpus() IPI to some CPU times out
  -> apps watchdog stops getting pets
  -> NON_SECURE_WDT bark at ~11 s (DT qcom,bark-time)
  -> hard reset, no kernel record
```

`glReadPixels` merely forces a real wait on a path that is already degraded.

## Why this was invisible for so long

Image 181's own summary is the answer: the watchdog "converts every observation
into a reboot", so for ~16 images the project was debugging a GPU problem
through a fuse that destroys the evidence. That is exactly the failure mode that
produced "the GPU kills the phone with no kernel record".

## Live confirmation still missing

I have **not** yet caught a `Watchdog bark!` line in the current boot's log, so
the last link (IPI timeout -> bark -> reset) is inferred from the archive and the
identical jiffie signature, not observed directly this session. The phone is
currently up and idle, which is consistent with a load-dependent trigger.

## Next step

The fix is not in the GPU driver at all — it is in the watchdog/pet path:

1. Get the bark line. Force the condition (GPU up + sustained CPU work) and
   capture `dmesg` live, looking for `Watchdog bark!`, `DW161-IPIX`,
   `alive_mask=`. The `/root/` capture from `bootcap.service` is the right
   channel; it survives the reset.
2. If confirmed, the fix is to stop `ping_other_cpus()` from being able to wedge
   a pet, or to make the bark non-fatal during bring-up testing. Images 173-181
   hardened this repeatedly without success, so the next attempt should target
   the root of the IPI timeout rather than adding more timeouts.

Do **not** revert the image-202 coherency fix. It is proven 3-for-3 and the
bring-up is now reliable — that was the real bug, and it is fixed.
