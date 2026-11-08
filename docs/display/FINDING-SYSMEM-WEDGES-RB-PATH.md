# Image 261b: the sysmem run freezes the GPU — and that is itself evidence

Date: 2026-11-16.

## What happened

The re-run with `FD_MESA_DEBUG=sysmem` completed the harness
(`MARK post-finish ... gl_err=0x0000`) and emitted its 64 `TRC` lines — then
the device stopped enumerating on USB and the user had to reboot manually.

This is the known hazard documented at `msm_drv.c:1208-1218`:

> "submitting GL work hard-resets the system with NO kernel record at all: no
> panic, no oops, no SMMU fault, no watchdog bite. dmesg simply stops
> mid-stream."

So the `TRC` data was lost with the ring buffer. **The capture method was the
defect, not the experiment.**

## Why this run matters anyway

The default (GMEM) path has now completed several times without wedging. The
**sysmem** path is the first configuration that froze the device.

Those two paths are not the same code in Mesa:

- GMEM: `fd6_gmem.c:1049` `emit_clears()` clears into **GMEM** — an on-chip
  carveout, no RB store to system memory.
- sysmem: `fd6_blitter.c:796` `fd6_clear_surface()` emits a real **RB blit**
  (`CP_BLIT` + `RB_BLIT_BASE_DST` + `GRAS_2D_DST_TL/BR`) that must reach DDR.

**sysmem is the only configuration that actually exercises the RB.** And the
one configuration that exercises the RB is the one that hangs the GPU.

That is a genuine, new localisation and it lines up exactly with the image-239
`rbobs_poll` note in this tree:

```
RB never, CP/SP -> backend never engaged (dispatch fork; CP_ME_INIT / GMU AO CGC are next)
```

We have independently established that the CP and SQE run fine and the stream
retires. What we have never seen working is the **RB**. And the one path that
demands the RB is the one that hangs.

This is consistent with — and stronger than — the earlier observation that the
clear colour never lands in any submitted buffer even under `sysmem`, where it
by design must.

## Fix for the capture (must be done before any re-run)

All `TRC` data must go to a **file**, not dmesg, because a hard reset destroys
the ring buffer:

```sh
dmesg -W > /root/trc-<tag>.txt 2>&1 &     # follow-NEW, no backlog
# ... run the experiment ...
kill -TERM $FW; ...
```

This is the `ndcap.sh` pattern that fixed the earlier lost-header bug. Using
`-W` (not `-w`) matters: `-w` first prints the entire ring backlog, and while
it does that the kernel's output overwrites entries it has not read yet.

Also: keep `cpstate_poller.py` running, since it fsyncs each sample itself and
therefore survives a reset.

## Next step

Re-run **sysmem** with the file-based capture, so the trace from the *wedging*
configuration is actually recorded. That is the run worth having: it is the
only one where the RB is involved, and it is the one that crashes.

Prediction to state up front:

- If the `TRC` burst shows the fence landing and then the ring/WPTR stopping
  advancing partway through the RB blit, the RB is hanging the GPU on the blit
  and the defect is in the RB/GRAS path, not in submission.
- If `TRC` shows normal completion and then the device dies, the hang is in the
  interrupt/power path after completion, not in the blit.

Either outcome is useful; what is not acceptable is losing the data again.

## Standing caution

Each sysmem run risks a manual reboot. Runs should be launched detached with
the poller up, and the user should expect to intervene.