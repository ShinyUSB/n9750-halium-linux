# Image 197 — the liveness gate works, and it corrects an over-claim

Kernel `#141`. Two fixes shipped, one of which matters more than the other.

## 1. Regression fixed: cpstate is readable AND writable again

Image 196 shipped `a6xx_gpu_cpstate` as `0440` to add a read path. That silently
stripped write permission and broke the *original* store path:

```
echo control > .../a6xx_gpu_cpstate  ->  Permission denied
```

The store path is what triggers every existing probe (CPDUMP, auto-rest,
auto-post), so losing it removed the only working register-dump mechanism on the
system. Image 197 is `0640`. Verified live: `-rw-r-----`, read returns `nogpu`,
write returns OK.

**Lesson:** adding a capability to an existing interface must not remove an
existing one. I broke the thing I was extending.

## 2. The liveness gate — the important fix

`harness/gpulive.py` replaces the `FD640`-only precondition with four conditions
that must **all** hold:

| condition | meaning |
|---|---|
| `GL_RENDERER == FD640` | driver bound to the real chip |
| `sw_rb_iova != 0` | `hw_init` actually ran |
| `CP_RB_CNTL == 0x20c` | ring programmed into hardware |
| `clocking == 1` | CP perf counter advancing |

Live result on `#141` with the GPU "up":

```
VERDICT: NOT SAFE TO TEST. FD640 alone is insufficient:
  - sw_rb_iova is 0x0 -> hw_init did NOT run
  - CP_RB_CNTL is 0x00000000, expected 0x20c -> ring not programmed
  - clocking != 1
```

**Mesa reports `FD640` while the GPU power domain is off and `hw_init` never
ran.** This is the same illusion that produced the "firmware ignores HFI"
dead end across images 191–194. It also means any earlier result gated on
`FD640` alone needs re-examination.

## 3. Correction: image 195's success was NOT stable

Image 195 log showed:

```
HFI-ACK id=1 error=0   HFI-ACK id=4 error=0   HFI-ACK id=3 error=0
verify_fw_version ret=0        CP is CLOCKING
```

Image 141, same code path:

```
fw_start end ret=0
HFI: verify_fw_version ret=-110          <-- timeout is BACK
hw_init suppressed after GMU boot error -110
```

So the missing `wmb()` was **necessary but not sufficient**. The HFI exchange
succeeds intermittently, not reliably. My "THE GPU IS ALIVE" headline on 195
described one lucky boot, not a fix. I should have said so at the time, and I
did not.

## Current honest state

| | |
|---|---|
| Bring-up (fw_start, BABEFACE) | ✅ reliable |
| HFI exchange | ⚠️ **intermittent** — works sometimes, times out other times |
| Ring programmed / CP clocking | ⚠️ only on the boots where HFI completes |
| GPU stays up long enough to test | ❌ |
| Pixels | ❌ |

## Next step

Stop treating the HFI timeout as a solved problem. The `wmb()` removed a real
ordering bug; something else is still making the GMU miss the queue. Given
`GMU2HOST_INTR_INFO = 0x8000` is observed on a failed boot (the DCVS ack bit
never cleared) and `SPTPRAC_PWR_CLK_STATUS = 0xc0` persistently, the power/
sequencer path that both Muse Spark and Ling flagged is now the leading
candidate again — and unlike when they said it, we now have a trustworthy
liveness gate to tell a working boot from a dead one.