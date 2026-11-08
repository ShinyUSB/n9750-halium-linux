# Image 209 results — the freeze was a duplicate trigger, not the diagnostics

Date: 2026-11-11. Evidence: `captures/freeze209-last_kmsg.log`.

## What actually happened

The phone was brought up **twice** in the same boot:

```
[  60.250383] bash:1151  a6xx: on-demand load handed to a6xx-load thread   <- trigger 1
[  60.316208] a6xx-load:1156  A6XX_TRACE: gpu->funcs->hw_init returned (ret=0)
[  73.013411] a6xx-load:1156  CPDUMP[auto-post] ---- end ----               <- trigger 1 completed fully

[ 140.391859] bash:1217  a6xx: on-demand load handed to a6xx-load thread   <- trigger 2
[ 140.459145] a6xx-load:1222  RINGHW: rb_iova=0x1000 rb_size=32768 sw_wptr_dw=0
[ 140.459225] a6xx-load:1222  RINGHW[05] a6xx_cp_init enter               <- and NOTHING after
```

Trigger 2 re-ran the full `adreno_hw_init` on a **live, already-initialised
GPU**, got as far as `a6xx_cp_init enter`, and never emitted another line. The
system then hung — the log simply stops at t=508.4 during ordinary background
work, with no panic and no oops.

So the hang is a **wedge inside the second `hw_init`**, not a crash, and not
caused by the added diagnostics. My earlier guess that the new `pr_info` calls
re-triggered the images 179–181 `console_unlock` wedge was **wrong**; the
diagnostics printed fine on both bring-ups.

A separate, unrelated oops did occur at t=112.267 and the system **survived it**:

```
kernel BUG at drivers/soc/qcom/rpmh.c:209!
Workqueue: ufs_clk_gating_0 ufshcd_ungate_work
pc : rpmh_write_batch+0x46c/0x47c
qcom_rpmh_mailbox HW IRQ 37 is PENDING at GIC
qcom_rpmh_mailbox Tasklet is not active
```

That is UFS storage clock gating via RPMh — nothing to do with the GPU. The log
continues for another 396 seconds afterwards. Noted, not pursued.

## The oops killed nothing, and no submit ever happened

```
RINGCPU lines : 0
HANGCHK141    : 0
```

**The ring dump was never reached**, because the run froze during bring-up and
never got to a GL submit. So image 209 produced *no* new evidence about the ring
contents. The measurement has not actually been performed yet.

## What we did learn (hard numbers)

```
RINGHW[04] memptrs=ffffff8019d9d000 memptrs_iova=0x0
          rb_memptrs=ffffff8019d9d000 rb_memptrs_iova=0x0
RINGHW[05] rbmemptr(rptr)=0x0  rbmemptr(fence)=0x4
RINGHW[06] gpu aspace=ffffffc366aeba00 mmu=ffffffc367272a00
          gmu_domain=ffffffc366aeb140
```

1. **`memptrs_iova == 0` is now confirmed on three independent boots** (208, 205
   baseline, 209), with the CPU pointer and the IOVA printed side by side for
   the first time. `rbmemptr(ring, fence) == 0x4`, so the `CACHE_FLUSH_TS` event
   is told to write the fence at IOVA 0x4.

2. **The GPU aspace and the GMU domain are distinct objects**
   (`aspace=...aeba00` vs `gmu_domain=...aeb140`), and `memptrs` and `rb_memptrs`
   are the *same* allocation (`...19d9d000`), confirming the wiring at
   `adreno_device.c:236` is correct.

3. The IOVA-0 overlap with the GMU `itcmhole` window (`iova=0x0 size=0x4000`)
   remains a real oddity, but with separate domains and zero SMMU faults it
   still does not look like the cause.

## Why two triggers fired

`load_gpu_real()` already guards with `if (!priv->gpu)`, so a second trigger
should be a no-op. It was not — the second thread ran the full bring-up. That
guard is therefore **not working**, and re-running `hw_init` on a live GPU
wedges the machine. That is a genuine robustness bug worth fixing, but it is
not the bug under investigation and it is not what the current image was for.

Most likely the duplicate came from my own harness: the first command
(`echo 1 > a6xx_gpu_load; sleep 30; ...`) hit the 240 s tool timeout while the
phone was unreachable, and the trigger was evidently delivered twice. Either
way, the lesson stands: **issue the GPU trigger exactly once, and never inside a
command that can time out.**

## Next step

Re-run image 209 — it is fine as built — with these changes to procedure only:

1. Trigger the GPU **once**, in its own short command, with no `sleep` long
   enough to hit a tool timeout.
2. Confirm `BABEFACE` before doing anything else.
3. Then run `fbprobe.py` to get a real submit, and capture `RINGCPU`.

`GL_RENDERER == FD640` remains the gate before any stage result counts.

## Open, unchanged

`ROOT-CAUSE-MISSING-SMMU-TABLE-UPDATE.md` stands exactly where it did. The
ring contents have still never been read, so the central fork — CPU side correct
(GPU-side translation gap) versus ring coherency — remains untested.
