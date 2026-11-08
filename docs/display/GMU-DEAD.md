# IMAGE 222 — the GMU acknowledges NOTHING. Both OOBs time out with INTR_INFO=0x0

Date: 2026-10-12
Image: `artifacts/flash/222-halium-boot-a6xx-gmulive.img`
Boot: `9cd5e0d7-a163-4697-9014-967b6c2f7ae6` (the run that succeeded)
Captures: `artifacts/gpu-bringup/captures/222-preboot.txt` (8181 lines, pre-trigger)

## The measurement

```
[382.408618] OOB GPU_DCVS: SET_MASK=0x800000   ACK_MASK=0x80000000  CLEAR_MASK=0x80000000
[382.408630] POST_START: oob GPU_DCVS ack-wait enter
[382.418688] POST_START: oob GPU_DCVS ack-wait TIMEOUT (INTR_INFO=0x0)
[382.418715] OOB132: set GPU_DCVS ret=-110          <- -ETIMEDOUT

[382.418950] OOB GPU_SET:  SET_MASK=0x10000   ACK_MASK=0x1000000   CLEAR_MASK=0x1000000
[382.418959] POST_START: oob GPU_SET ack-wait enter
[382.428984] POST_START: oob GPU_SET ack-wait TIMEOUT (INTR_INFO=0x0)
[382.429011] OOB132: set GPU_SET ret=-110           <- -ETIMEDOUT
```

**Both** out-of-band requests fail. `GMU2HOST_INTR_INFO` reads **exactly 0** —
not one bit, not even the stray `0x10000000` seen in an earlier boot. The GMU
is not raising interrupts at all.

`VOTE151: gx=0x9 mx=0x6` immediately before this shows the host did write the
GX vote; the GMU simply does not act on it.

## What this establishes

| layer | state |
|---|---|
| GMU firmware **image** | byte-identical to stock (sha256 `3ff0c027…`), **verified** |
| GMU firmware **execution** | **not responding to anything** |
| GX clock domain | **OFF** (`gpu_cc_gx_gmu_clk en=0`, `gcc_gpu_gpll0_clk_src en=0`) |
| CX clock domain | ON — CP runs, ring fetches work, `CP_EVENT_WRITE` lands |
| Result | RB/SP never clocked → no blit, no render, no pixels |

The GMU firmware is the component that programs the RPCS/ARC commands which
switch on the GPU's GX power rail. It is loaded correctly and is **completely
unresponsive**. That is why the GX domain never comes up, and why nothing is ever
written to memory — while every command, address and buffer we have checked is
correct.

This is now the tightest explanation we have, and it is consistent with every
previous observation including the intermittent freezes: a GMU that is
partially alive but not servicing OOB would produce exactly the marginal,
sometimes-works behaviour we saw (360s trigger OK, 385s trigger freeze).

## Secondary finding: the GMULIVE poller never fired

`GMULIVE` (the `HFI_VERSION_INFO` read I added for image 222) **did not print**,
despite being present in the delivered kernel (`strings vmlinux | grep GMULIVE`
confirms it) and the work being armed:

```
[382.418793] GVIT157: poller armed (first +5s, then 30s)
...nothing for the following 120+ seconds...
```

Ruled out: work not cancelled (no `cancel_delayed_work` anywhere), system
healthy (uptime advancing, CPU idle), dmesg not wrapped (background logger on
the device also shows 0), firmware present (`strings` verified).

`gvitals157_fn()` returns early when `gvitals157_gmu` is NULL, but that pointer
is assigned one line before the schedule in `a6xx_gmu_resume()`. So either the
work is not running at all, or `gvitals157_gmu` is being cleared. **This is a
bug in my own diagnostic, not a finding about the GPU.** The poller has worked in
earlier boots (GVIT157 lines at t=1789s+ in an earlier run), so this is
intermittent too.

`HFI_VERSION_INFO` therefore remains unread. It is still the single most
informative register we have not read.

## Revised understanding of the freezes

The "trigger before 300s" rule is **insufficient**: 385s also froze. The
freeze is not a timing rule to be respected — it is most likely a *symptom of
this same GMU fault*, since a GMU that intermittently fails to service OOB
would produce exactly this marginal behaviour.

## Next moves

1. **Fix the poller** — this is now the blocker. Print unconditionally at the
   top of `gvitals157_fn()` (before the NULL check) and re-arm with
   `schedule_delayed_work()` regardless of `gmu`, so we can distinguish
   "work not running" from "gmu NULL". Cheap, and it unlocks
   `HFI_VERSION_INFO`.
2. **Read `HFI_VERSION_INFO` from the OOB timeout path** instead of the poller —
   print it inline at the point of failure, which we know executes. That avoids
   the poller entirely.
3. If `HFI_VERSION_INFO` reads `0x00000000` or `0xffffffff`, the GMU firmware is
   not executing and the problem is in how it is started (boot_slumber
   handshake, AOSS/RPMH interaction, or the `legacy` flag split at
   `a6xx_gmu.c:1310`).
4. Only then revisit the GX clock — with the GMU dead, force-enabling
   `GPU_CC_GX_GMU_CLK` is the fallback, but it would mask the real fault.

## Method note

The background-logger trick worked well: the phone survived this run, and we
captured the entire bring-up window. On the previous run the phone wedged before
anything could be read. **Always start an on-device `dmesg > file` loop before
triggering the GPU**, not after.