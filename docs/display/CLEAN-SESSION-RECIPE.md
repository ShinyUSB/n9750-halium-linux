# Clean-session recipe (image 245)

Getting a *trustworthy* measurement on this device took most of the session.
This is the shortest path to one.

## The three things that must be true

1. **Runtime PM is pinned on, before the GPU is ever used.**
   Otherwise `a6xx_pm_suspend()` blocks partway, the device sticks at
   `runtime_status=suspending` with the GPU **half powered down**, and every
   draw is silently discarded. That is what invalidated images 235–244.

2. **The GPU is genuinely powered**, confirmed by `SPTPRAC_PWR_CLK_STATUS == 0`
   and `RPMH_POWER_STATE == 0` in *every* sample.

3. **Samples are written to disk**, because a hard freeze destroys the ring
   buffer and writes no pstore.

## Steps

```sh
# from the host
./u.sh --fix

# on the phone, immediately after boot, BEFORE any GPU use:
echo on > /sys/devices/platform/soc/2c00000.qcom,kgsl-3d0/power/control
#   -> prints "control=on status=unsupported"   (unsupported is correct/expected:
#      the GPU has not been used yet)

# start the disk poller
nohup /root/poller.sh >/dev/null 2>&1 &

# *** WAIT UNTIL UPTIME > 300 s BEFORE THE NEXT LINE ***  (see below)
while [ "$(cut -d' ' -f1 /proc/uptime | cut -d. -f1)" -le 320 ]; do sleep 15; done

# now the GPU is safe to use
echo 1 > /sys/devices/platform/soc/ae00000.qcom,mdss_mdp/a6xx_gpu_load

# run any GL harness DETACHED (a real draw can freeze the machine)
nohup sh -c "timeout -s KILL 90 python3 /usr/local/bin/wrtest.py > /root/wrtest.out 2>&1" &
```

### THE 300 s RULE — do not skip it

`a6xx.gpu_init_delay_ms=300000` (in the cmdline) defers GPU init by five
minutes. **Writing `1` to `a6xx_gpu_load` before uptime exceeds 300 s
FREEZES the device.**

> "The real cause: **`a6xx.gpu_init_delay_ms=300000`** (5 minutes)."
> "**Always wait until uptime > 300 s before writing `a6xx_gpu_load`."
> — `session-929cd597-…md:6925-6934`, `IMAGE-221-CLOCKS.md` §0

Past 300 s the deferred init still has *not* fired on its own, so the write is
still required — you must both wait **and** write.

This is not a theoretical hazard. It was missed in this recipe until
2026-10-18, and the symptom is not a clean failure: the device wedges, stops
answering ping/SSH, and may come back as a kernel panic in
`rpmh_write_batch+0x46c` (an RPMh request that AOSS never answers, because the
GPU is wedged). That panic was long misread as an independent, pre-existing UFS
LPM bug. It is a *consequence* of the early GPU bringup. See
`REBOOT-CAUSE-RPMH-BUG.md` and `FINDING-SHADER-ISA-NEVER-SUBMITTED.md`.

### Other storage caveats

- `/` is a 2.0 GB loop0 image and is normally **100 % full**. `poller.sh`
  writes to `/root/cpsamples.txt`, i.e. onto that full filesystem. Point it at
  `/userdata` instead, or delete the file afterwards.
- Put all harness output on `/userdata` (230 GB, 224 GB free).
- `poller.sh` spawns `cut`/`cat` in a loop, and each exec floods dmesg with
  Samsung's `five-diag:` lines, which evicts the dmesg ring buffer — including
  the very evidence you are trying to capture. Stop the poller before a
  measurement, and capture with `dmesg -W > /userdata/...` instead.

Afterwards, pull the results:

```sh
./u.sh 'cat /root/wrtest.out; echo; grep -c "^=== t=" /root/cpsamples.txt'
./u.sh 'grep "^SPTPRAC_PWR_CLK_STATUS " /root/cpsamples.txt | sort | uniq -c'
./u.sh 'grep "^CP_INTERRUPT_STATUS "   /root/cpsamples.txt | sort | uniq -c'
```

## Reading the samples

| register | healthy value | if you see otherwise |
|---|---|---|
| `SPTPRAC_PWR_CLK_STATUS` | `0x00000000` | anything else → GPU not fully on, result invalid |
| `RPMH_POWER_STATE` | `0x00000000` | nonzero → GPU in a low-power state, invalid |
| `CP_RB_WPTR` vs `CP_RB_RPTR` | equal | W ahead of R → ring backed up |
| `CP_TIMESTAMP ... clocking` | `1` | `0` → CP not fetching, everything invalid |
| `CP_INTERRUPT_STATUS` | `0x00000000` | nonzero → an actual fault; **read it, do not dismiss it** |

A useful cross-check: `CP_TIMESTAMP delta` should be a healthy nonzero value
(~675,000 in the last good session). If it is 0 or flat, the GPU is not running
and any "all zeros" result is meaningless.

## Known hazards

- **A real draw can hard-freeze the phone**, intermittently. Recover by manual
  reboot only. Always launch detached with the poller running.
- `cpstate` writes (the `ring()` helper in `drawprobe.py`) return **mangled**
  `wptr` values — observed incrementing by a constant +15 per call, which is
  not a real ring pointer. **Do not use `wptr` as a submit signal.** Use
  buffer contents or `CP_RB_WPTR/RPTR` from the poller instead.
- dmesg only retains a few minutes. Anything you need must be captured live.