# ROOT CAUSE — `wptr==0` solved: the HFI vote *probe* fails, the retry then poisons hw_init

Kernel `#135` (image 193). Found by reading `cap193.log` line by line through the
7.5 s gap between `fw_start` succeeding and the resume returning `-110`.

## The chain, exactly as logged

```
[ 50.481878] A6XX_TRACE: enter a6xx_pm_resume
[ 50.486727] CAPTURE E-babeface ... HFISTAT=0x1        <- firmware booted OK
[ 50.593522] POST_START: fw_start end ret=0            <- ATTEMPT 0 SUCCEEDED
...
[ 58.100380] platform 2c6a000.qcom,gmu: GMU firmware initialization timed out
[ 58.100403] A6XX_TRACE: after GMU start (ret=-110)    <- ATTEMPT 1 FAILED
[ 58.110490] POST_START: oob GPU_DCVS ack-wait TIMEOUT (INTR_INFO=0x0)
[ 58.110490] BC132: ev=RESUME_RETURN ret=-110 depth=6
[ 58.110551] POSTINIT[00] ENTER_HW_INIT_WRAPPER needs_hw_init=1
[ 58.110557] A6XX_DIAG: hw_init suppressed after GMU boot error -110; staying alive
```

Why was there an attempt 1 at all? Because of this in the retry loop
(`a6xx_gmu.c:1818-1825`):

```c
if (gmu_hfi_probe || experimental_nonlegacy)
        ret = a6xx_gmu_hfi_vote_probe(gmu);

if (!ret)
        break;
next_attempt:;
```

**Attempt 0 brought the GMU up perfectly** (`fw_start ret=0`, `GOOD BABEFACE`,
`HFISTAT=0x1`). Then a **diagnostic HFI vote probe** — an instrument, not a
bring-up requirement — returned non-zero. That sent the loop round again.
Attempt 1 re-ran `fw_start` on an already-running GPU, which timed out at
`-110`. That `-110` propagated out of resume and latched
`a6xx_gmu_boot_error`, and `msm_gpu_hw_init()` skips hw_init while it is set.

So the ring was never programmed — `rb_iova=0x0`, `RB_BASE=0x0`, no `RINGHW[]`
lines, "CP is NOT CLOCKING" — while `GL_RENDERER` read `FD640` and every
`GEM_SUBMIT` returned 0. **That is the whole reason `wptr_dw` stayed 0.**

## Two wrong conclusions I made on the way (recorded)

1. **"sptprac poll TIMEOUT caused the -110."** Wrong. `a6xx_sptprac_enable()`
   always `return 0` on timeout (`a6xx_gmu.c:800-805`); it only logs. Evidence:
   `fw_start end ret=0` is timestamped 96 µs *after* the sptprac TIMEOUT line.
   I built image 194 around making that call non-fatal, then found the `if (ret)
   return ret` was already dead code, and reverted it. **The image-194 sptprac
   change was a no-op and is not in the flashed image.**

2. **"the -110 came from the DCVS OOB ack timeout."** Wrong, and backwards in
   time. `OOB GPU_DCVS ack-wait TIMEOUT` is printed at `58.110490`, i.e. *after*
   `after GMU start (ret=-110)` at `58.100403`. The DCVS timeout is a
   **consequence** of the already-failed retry, not its cause. Also note
   `a6xx_gmu_set_freq()` ignores its own OOB return value
   (`a6xx_gmu.c:187-188`), so it could not have set `ret` anyway.

I chased both because each was the most recent thing in the log. Reading the
whole window in order rather than grepping for suspects is what found it.

## The fix

Do not let a diagnostic probe failure trigger a full re-boot of a healthy GMU.
The probe must not be able to fail the bring-up.

## Honest status

- **Proven:** the sequence above, read directly from the log.
- **Unproven:** whether `hfi_vote_probe` fails for a benign reason (e.g. no
  `nr_gpu_freqs` index to vote, so the vote is meaningless). Its own body has
  `GX132-PROBE-SKIP ... return 0` for that case, so it did take the real path.
- **Not yet done:** the actual code change. I reverted the wrong fix and had not
  yet written the right one when this record was written.

Images 190/191 (submit path) and 192 (ITCM hole) remain verified-good and are
not implicated. Image 193's vblank suppression is unrelated to this chain.