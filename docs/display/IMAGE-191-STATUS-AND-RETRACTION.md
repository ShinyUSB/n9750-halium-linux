# Image 191 status — forensics loss, a retraction, and the real current blocker

Date: 2026-10-10. Phone booted image 191 as kernel `#133`.

## 1. We lost the freeze forensics, and my capture mechanism was the wrong one

The `#133` boot froze ~49 min in (clock stopped; re-plugging USB changed nothing
because the whole system was wedged). Forensics were expected to come from
`/proc/last_kmsg` via crashwatch. They did not, for two compounding reasons:

**(a) A hard freeze writes no pstore record.** `last_kmsg` is only populated via
the clean panic/reboot path. There is no panic on a hard freeze, so there is
nothing for pstore to save. Verified: the `last_kmsg` on the next boot was
stale — first line `00 00000000`, last line `Exit EBS [ 3418] UEFI End`, i.e. a
boot with 3418 s uptime ending in UEFI, hours before the freeze.

So the ring buffer is structurally the wrong place to keep evidence for
freezes. This is a mechanism error, not bad luck.

**(b) crashwatch had been silently dead since 23:42.** `.opencode-tmp/crashwatch.sh`
hardcoded `DEVICE_IP=192.168.1.16`, an address abandoned when we moved to USB
RNDIS (`10.15.19.82`). It had been polling a dead address and archiving nothing
for several boots. Fixed to try both addresses and repair the host-side RNDIS
address each pass.

**The fix:** `harness/gpu-capture.sh` streams `dmesg -w` to a file *on the
phone* (`/var/log/gpu191.log`). Unbuffered per-message writes, because a
block-buffered pipe loses the last several KB on a hard freeze — precisely the
lines that matter. It also runs a 5 s `MARK` loop recording `wptr`, GPU IRQ
count, and fault count, so progress survives even if the log stream is cut.

This log is what produced every finding below.

## 2. RETRACTION — my "191 worked" claim was wrong

I reported that stage 6 completed with no `invalid flags` rejection and called
that proof image 191 was accepted. **That was wrong, and I retract it.**

The strace of that very run shows:

```
ioctls=44
GEM_SUBMIT lines:  (none)
SUBMITQUEUE lines: (none)
```

**There was no submit at all.** A single `glClear` on the default framebuffer is
*deferred* by freedreno (`should_defer` in `freedreno_ringbuffer_sp.c`) and never
reaches the kernel. So the absence of `invalid flags` proved nothing — absence of
an error is not acceptance when there was no attempt.

The same mistake recurred one step later: `hugedraw` printed `HUGE-DRAW-DONE`
with no submit, and I initially read that as success. It was **llvmpipe**. With
`FD_MESA_DEBUG=msgs` the driver says so plainly:

```
MESA: error: get_param:232: get-param failed! -6 (No such device or address)
MESA: error: fd_pipe_new2:52: allocation failed
MESA: info:  1237: fd_screen_create:1009: could not create 3d pipe
```

`ENXIO` from `get_param` is the GPU-not-loaded signature. **Image 191 is still
untested.**

The lesson, which I should have applied the moment a clean run appeared: on this
project a passing result is worthless unless the same run proves it used the GPU.
Every check must assert `GL_RENDERER == FD640` *in that same run*.

## 3. The actual blocker: `fw_start` times out at -ETIMEDOUT

Bring-up starts and never finishes:

```
POST_START: fw_start end ret=-110
A6XX_TRACE: fw_start returned (ret=-110)
OOB132: clear state=0
A6XX_TRACE: a6xx_gmu_resume RETURNING ret=-110
BC132: ms=1431636719 ev=RESUME_RETURN ret=-110 depth=9
RECOVER166: end ret=-110
```

`-110` is `-ETIMEDOUT`. And the state poll agrees:

```
GVIT157: SPTPRAC=0xc0 RPMHPWR=0x0 GXVOTE=0x0 MXVOTE=0x0
         OOB_H2G=0x80800000 OOB_G2H=0x8000 hfi_irq=0 gmu_irq=1
```

- `gmu_irq=1` — the GMU raised an interrupt that was never serviced
- `hfi_irq=0` — no host-firmware-interface handshake arrived
- `RPMHPWR=0x0` — RPM sees the GPU as powered off

So the GPU's GMU firmware never completes its handshake with the host, and
`fw_start` waits forever. This is the "lottery" we have been fighting, and it is
a *timeout*, not a rejection — the firmware is not answering at all.

## 4. Why nothing could be tested

The GPU did come up exactly once this boot (stage 3 returned
`renderer=b'FD640'`), then dropped again. Every subsequent test hit `ENXIO`.

The ssh round-trip between detecting the GPU and running the test is long enough
that the GPU usually drops inside it. That is precisely how a llvmpipe
`HUGE-DRAW-DONE` got mistaken for a freedreno success.

**`harness/test191.sh` fixes this by running entirely on the phone:** loop the
bring-up trigger, check the renderer, and the instant it reads `FD640`, run
`hugedraw` under strace with no intervening network hop. It asserts `FD640`
before trusting any result.

## 5. Where things stand

| item | status |
|---|---|
| image 190 (DRM version 4, `sp_funcs`) | **confirmed** — NULL-deref flush crash gone, submitqueue opened |
| image 191 (`MSM_SUBMIT_BO_DUMP`) | **built, untested** — cannot reach a submit without bring-up |
| bring-up itself | **flaky** — `fw_start` `-ETIMEDOUT`, `gmu_irq=1` unserviced |
| forensics for the freeze | **lost** — no pstore record exists; new on-device capture now in place |

Next step is `test191.sh` running the lottery. If the GPU stays down, the
question to answer is why `fw_start` times out and why the GMU IRQ is never
serviced — that is upstream of everything 191 could fix.