# 🎉 Image 189 — THE GPU CORE IRQ FIRED. `wptr_dw` moved. The GPU executed commands.

Kernel `#131`. This is the first time in this project that the GPU has
executed work we submitted. Three separate long-standing zeros all broke at
once, from one-line change.

## What the fix was

`msm_gpu_submit()` deadlocked in `rpm_resume` (stack captured live, see
`ROOT-CAUSE-SUBMIT-RPM-DEADLOCK.md`). Removing the unbalanced
`pm_runtime_get_sync()` — the same remedy as image 184 — is the entire change.

## The proof

Same harness (`harness/submitqueue.py`) that hung in D forever on `#130`:

**Before (on `#130`):**
```
GEM_SUBMIT  ->  D (disk sleep), had to kill -9
stack: __switch_to -> rpm_resume -> __pm_runtime_resume -> msm_gpu_submit
```

**After (on `#131`):**
```
GEM_NEW      rc=0  handle=1 size=4096
GEM_INFO     rc=0  offset=0x102166000
SUBMITQUEUE_NEW  id=1
GEM_SUBMIT   rc=0  returned fence=1
*** SUBMIT ACCEPTED BY THE DRIVER ***
```

### The ring moved

```
CPDUMP[sq0]  sw: wptr_dw=99   next_dw=99      <-- before
CPDUMP[sq1]  sw: wptr_dw=114  next_dw=114     <-- AFTER the submit
```

**`wptr_dw` went 99 → 114.** For 189 images that number had never been
anything but 99. Not for clears, not for 1M-vertex draws, not for shaderless
draws, not for textured draws.

### The CP consumed it, and the IRQ fired

```
CPDUMP[sq1] hw: RB_RPTR=0x72  RB_WPTR=0x72
CPDUMP[sq1] st: RBBM_STATUS=0x800005   irqs=3
```

- `RPTR == WPTR == 0x72` (=114). The command processor **read every dword we
  wrote**. The ring drained.
- `RBBM_STATUS` changed from `0x1` to `0x800005` — the GPU actually did work.
- **`irqs=3`**, and live:

```
487:  3  ...  PDC-GIC 332 Level  A640      <-- core GPU IRQ: FIRED
488:  7  ...  PDC-GIC 336 Level  kgsl_hfi_irq
```

**The GPU core IRQ has now fired. Its count was 0 for the entire project.**
That is the signal we were chasing since image 186, and it turned out not to be
misconfigured at all — it simply had nothing to report until a submit could
get past `rpm_resume`.

## What is fixed and what is not

**Fixed (driver):** `msm_gpu_submit()` no longer deadlocks. A valid submit now
reaches `a6xx_submit()`, is encoded into the ring, executed by the CP, and
raises the completion interrupt. The full driver-side submit path is proven
end-to-end for the first time.

**Still broken (userspace):** freedreno *still* issues zero `GEM_SUBMIT`.

```
ioctls=206   GEM_SUBMIT=0   SUBMITQUEUE=0
```

So the two-bug split I stated before flashing is exactly what happened. The
driver is now capable of submitting, and freedreno still never asks it to.

## Why this is progress that compounds

The remaining userspace bug is now isolated to a *much* smaller surface than it
looked an hour ago. Previously every hypothesis had to explain both "no submit"
*and* the mysterious zero IRQ. Now:

- The IRQ mystery is **resolved** — it was downstream of the deadlock.
- `INT_0_MASK` was always correct (bit-exact `A6XX_INT_MASK`); that dead end is
  definitively closed.
- The CP was always healthy; the ring was simply never written.
- Everything remaining lives in freedreno's Gallium path between
  `fd_context_draw` and `msm_submitqueue_flush`, and the kernel will now
  service whatever it sends.

And critically: **the moment freedreno does issue a submit, it will now work.**
There is no longer a second blocker behind it. That is a very different
position from where we started this session.

## Verified state

- Build: 0 errors. `.config` byte-identical to previous images
  (`cd401ed6…`) — no config drift, all four watchdog fuses still off.
- Package marker check reads 0 for the 189 string, and that is *correct*: the
  fix is a removal and comments are not compiled into the binary. There is no
  `pr_info` to find. Behaviour on device is the only verification, which is
  what the sq0/sq1 pair provides.
- Phone alive on `#131`, GPU loaded, reachable over USB RNDIS.

## Next

Re-run the freedreno side. `wptr_dw` is now a live progress indicator in a way
it never was: it moves when the kernel is fed, so any future submit from any
client is immediately visible. The remaining work is one bug, in one place,
and it is now the *only* thing between us and the GPU executing our pixels.