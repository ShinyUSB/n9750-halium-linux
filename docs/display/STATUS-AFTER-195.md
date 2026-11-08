# Status after image 195 — bring-up is fully solved; the failure moved downstream

Kernel `#139`. Read from the durable on-device capture (`cap195.log`), which
survives the reboots that the ssh link does not.

## Confirmed good — the whole bring-up path now succeeds

```
[64.710659] A6XX_TRACE: gpu->funcs->hw_init returned (ret=0)     <-- was never reached before
[64.710706] A6XX_TRACE: msm_gpu_hw_init RETURNING ret=0
[64.710756] CPDUMP sw : rb_iova=0x1000 rb_size=32768 wptr_dw=99 next_dw=99
[64.710772] CPDUMP hw : RB_BASE=0x1000 RB_CNTL=0x20c RB_RPTR_ADDR=0x0
[64.710787] CPDUMP hw : RB_RPTR=0x63 RB_WPTR=0x63 SQE_CNTL=0x1 SQE_BASE=0x9000
[64.710812] CPDUMP st : RBBM_STATUS=0x1
[64.956476] CPF: delta=165811436 => CP is CLOCKING (running)
```

This is the first boot in the project where:
- `hw_init` completes and returns 0
- `rb_iova` is non-zero (`0x1000`)
- `CP is CLOCKING`

`wptr_dw=99` is the historical "nothing submitted yet" marker — correct at
bring-up, before any GL context exists. It is the same value image 189 reached
with a hand-built submit, which then advanced it to 114.

## The failure has moved — and this is progress, not regression

Previously: GPU never came up -> GL fell back to llvmpipe -> silence.
Now: GPU comes up, ring is programmed, CP clocks, and **the system hard-resets
when GL work is actually submitted**.

Evidence:
- The capture log ends cleanly at `64.956489` with `---- end ----` — no fault,
  no SMMU error, no panic, no watchdog.
- No `GPU fault`, no `shader page fault`, no `ring overrun`, no `Unable to handle`
  anywhere in dmesg.
- The phone is up 386 s at one point with no reboot at all (image 193's vblank
  suppression is holding), then resets once the trigger runs.

So the reset is a **hard reset with no kernel record**, which is why
`/proc/last_kmsg` cannot capture it (same limitation as the earlier freeze) and
why ssh dies at exactly that moment. The durable capture is the only witness.

## Pixel readback harness: built and verified, not yet successfully run

`harness/pixelreadback.py` — deliberately avoids the display path (the panel is
broken on this tree) and tests an FBO we own:

1. clear to known colour, read back, compare
2. clear to a *different* colour on the same FBO — proves no stale buffer
3. scissored clear into a sub-rect — proves geometry is honoured
4. shader + draw — exercises the ROP, not the clear fast-path
5. corner coverage — proves the rasteriser placed geometry

It refuses to run unless `GL_RENDERER == FD640`, because on llvmpipe every check
would pass and mean nothing.

`harness/run-readback.sh` runs trigger + renderer check + readback entirely on
the phone and writes `/var/log/pixel-result.txt`, because driving it over ssh
loses the link (two "Connection reset by peer" failures).

**It has not yet produced a result.** Every attempt so far has been killed by
the reboot happening at exactly the point GL work is submitted.

## Honest status

| | status |
|---|---|
| GMU firmware boot | ✅ working |
| HFI transport | ✅ working (ACKs, `error=0`) |
| Ring programmed in hardware | ✅ `rb_iova=0x1000`, `RB_CNTL=0x20c` |
| CP clocking | ✅ |
| Mesa `GEM_SUBMIT` accepted | ✅ `= 0` |
| **Pixels rendered and read back** | ❌ **not yet demonstrated** |
| Stable across GL work | ❌ hard reset when work is submitted |

The next thing to investigate is the reset that now occurs when the CP executes
submitted commands. It produces no kernel log, so it needs a different evidence
channel than dmesg — most likely reading CP/GPU status registers from a
userspace probe *while* the work runs, or letting the capture record longer
around the submit.

Do not revert 192/193/195. All three are load-bearing and verified.