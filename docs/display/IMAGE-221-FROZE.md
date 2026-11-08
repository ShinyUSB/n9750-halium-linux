# IMAGE 221 — flashed; phone froze on GPU trigger

Date: 2026-10-12
Image: `artifacts/flash/221-halium-boot-a6xx-clkchk.img`
SHA256: 88509689e9040363d677535720a94576fa6c1e13042ac46c373bd28893480879
Boot: `c094db90-b388-408f-aeda-a20ea8539ece`

## What 221 contained

1. **`CLKCHK[seq=...]`** — clock/power readback on every submit, gated on `ibdump`:
   `RBBM_CLOCK_CNTL`, `CLOCK_CNTL2_RB0..RB3`, `CLOCK_CNTL2_SP0..SP3`,
   `CLOCK_CNTL2_UCHE`, `CLOCK_CNTL2_TP0`, `CLOCK_CNTL2_RAC`,
   `CLOCK_CNTL_SP0/SP1`, GMU `SPTPRAC_CLOCK_CONTROL`,
   GMU `SPTPRAC_POWER_CONTROL`, `RBBM_STATUS`, `CP_RB_CNTL`, `RB_CCU_CNTL`.

   Purpose: everything ever proven about "the GPU runs" is CP-domain
   (`CP_SCRATCH_REG(2)`, ring fence, `CP_EVENT_WRITE`). Blits and render passes
   are executed by RB/SP/UCHE. We have never shown those are clocked.

2. **Raised BOS dump cap 1024 -> 8192 dwords.**

   The old cap of 1024 dwords = 4096 bytes = `0x1000`, which is *exactly* the
   size of a UBWC metadata plane. For a 64x64 UBWC RGBA8 target the layout is
   `0x4000` colour + `0x1000` metadata, and the colour plane begins at `0x1000`
   — precisely where the cap stopped. **Image 220's "render target is all zeros"
   result was measuring the metadata plane and not one byte of colour data.**

## Run

- Flash: clean, BOOT-only, hash verified, ramdisk identical to 220.
- Boot: `c094db90`, responsive, dmesg banked (8247 lines) to
  `artifacts/gpu-bringup/captures/221-preboot.txt`.
- `ibdump` armed (`Y`).
- `echo 1 > .../a6xx_gpu_load` -> `TRIGGER_OK`.
- **Immediately after the trigger the phone stopped answering ARP.**
  `ping` 100% loss, `ip neigh` FAILED, but `lsusb` still lists `18d1:d001` and
  the link is UP. `u.sh` failed after all 6 attempts.
  Signature matches the established freeze-after-GPU-trigger pattern.

## Likely cause — my own doing (WRONG, retracted)

**First freeze, boot `c094db90`.** I attributed this to printk flooding, because
`ibdump` was armed *before* the trigger with the cap at 8192 dwords and
`CLKCHK` firing per-submit.

**Second freeze, boot `26271774`.** Retried with the *opposite* discipline:
`ibdump` left at its default `N`, dump never armed, nothing enabled before the
trigger. Sequence was: connect -> bank dmesg (8100 lines) ->
`echo 1 > a6xx_gpu_load` -> `TRIGGER_OK` -> next command fails, ARP FAILED,
ping 100% loss, `lsusb` still shows `18d1:d001`, link UP.

**It froze again. So the printk-flood theory is wrong and is retracted.** The
freeze is a genuine, reproducible consequence of the GPU load trigger itself,
not of the instrumentation volume. The relevant variable across these boots is
simply *whether `a6xx_gpu_load` was ever written*, and that reliably kills the
phone.

Note also that the freeze happens **after `TRIGGER_OK` returns**, i.e. after the
write succeeded and while the GPU is coming up — not during the echo.

## What this changes

- We have never once completed a GL workload on this kernel image. Every
  successful `FD640` + `glstage` result we have was obtained on a boot where the
  GPU was already loaded and the trigger was a no-op.
- The freeze is very likely part of the same underlying fault we are chasing:
  the GPU comes up enough to accept `a6xx_gpu_load`, then the system wedges when
  the hardware actually starts doing work (clock-domain / power sequencing).
  That is **consistent with, and independent support for, the RB/SP/UCHE clock
  hypothesis** — the same thing that would make every write a no-op could also
  wedge the SoC when real work is scheduled.
- It also means the `CLKCHK` readback cannot be obtained by loading the GPU on a
  live system. It must instead be read **during or immediately after
  `hw_init`, without ever enabling a GL workload** — i.e. moved into
  `a6xx_hw_init()`/`adreno_hw_init()` so it prints once at bring-up, then the
  phone is never asked to do GPU work at all.

## Next image should therefore

1. Move `CLKCHK` out of the submit path and into `hw_init`, printing **once**,
   unconditionally (no `ibdump`, no `seqno` gate).
2. Add read-back of the same registers immediately *after* `a6xx_set_hwcg(true)`
   as well, so we can see whether anything actually changed.
3. Do **not** trigger the GPU and do **not** run any GL workload on that image.
   Its sole purpose is a one-shot clock/power snapshot at bring-up.
4. Keep the raised BOS cap (it is needed later) but leave `ibdump=N` by default.

This gets us the clock data without ever asking the GPU to do work.

## Data not yet captured

The `CLKCHK` clock values — the entire point of image 221 — were never reached.
Needs a re-run. If the freeze was printk-flood-induced, gating the dump will
also fix that.

## Summary of state

Still open, and now genuinely narrowed:

- **PROVEN:** Mesa's nested IB executes (`CP_EVENT_WRITE` timestamps increment
  once per submit in control memory).
- **PROVEN:** the 2D blit stream is complete and correct, including
  `RB_DBG_ECO_CNTL = 0x04100000` (the A640 `BLIT_OP_SCALE` workaround)
  bracketing `CP_BLIT`.
- **RETRACTED:** "`0x26000` is unmapped" — it is `0x25000 + 0x1000`, the UBWC
  metadata-plane offset of a 5-page BO. `bos[]` lists bases; no defect.
- **RETRACTED:** "render target is all zeros / nothing lands" — image 220's cap
  of `0x1000` bytes covered only the UBWC metadata plane. Must be re-tested
  with the raised cap.
- **OPEN:** whether RB/SP/UCHE are clocked. Image 221 was built to answer this
  and did not get the chance.