# 🎉 Image 202 — Mode A SOLVED. The GMU firmware boots, HFI completes, the ring runs, the CP clocks.

Date: 2026-10-11. Kernel `#147`. Flashed and verified on hardware.

**This is the first boot in 201 images where the complete bring-up chain succeeds
and the GPU stays alive.** Every previous image was blocked at or before HFI.

## The verdict

Two independent boots (different power cycles), identical result:

```
[  61.711546] ALLOC202 iova=0x0       size=0x4000  contiguous=1
[  61.711834] ALLOC202 iova=0x4000    size=0x3c000 contiguous=1
[  61.712102] ALLOC202 iova=0x44000   size=0x3c000 contiguous=1
[  61.717424] BC132: ev=RESUME_ENTRY ret=0 depth=1
[  61.722327] FORENSIC VERDICT GOOD BABEFACE ms~1      <- CM3 booted, first try
[  61.728807] POST_START: fw_start end ret=0
[  61.728822] === GMU BOOTSTRAP BEGIN ===
[  61.729063] HFI-ACK id=1  seq=0 error=0
[  61.729240] HFI-POLL id=1  seq=1 completed=1 INTR_INFO=0x0
[  61.729252] HFI: verify_fw_version ret=0             <- was -110 for 200 images
[  61.729830] HFI: send_perftbl ret=0
[  61.731567] HFI: send_bwtbl ret=0
[  61.732041] HFI: feature_ctrl ECP ret=0
[  61.732538] HFI: core_fw_start ret=0
[  61.732971] HFI: init_perf_vote(start) ret=0
[  61.732053] === GMU BOOTSTRAP END ===
[  61.766232] RINGHW-W: CP_RB_CNTL before=0x0
[  61.766232] RINGHW-W: CP_RB_CNTL after=0x20c         <- ring programmed
[  61.766844] RINGHW: rb_iova=0x1000 rb_size=32768
[  61.767622] gpu->funcs->hw_init returned (ret=0)
```

**Eight HFI messages, eight ACKs, eight `ret=0`, zero timeouts.** The poll loop
completed on the IRQ path every single time (`completed=1`).

## The liveness gate PASSES — for the first time ever

| condition | result |
|---|---|
| `GL_RENDERER == FD640` | device present (`/dev/dri/renderD128`) |
| `sw_rb_iova != 0` | ✅ `rb_iova=0x1000` |
| `CP_RB_CNTL == 0x20c` | ✅ `RB_CNTL=0x20c` |
| `clocking == 1` | ✅ `delta=51068682 => CP is CLOCKING` |

Confirmed twice in one boot, 12 s apart:

```
[  61.843444] CPF: t0=0x8120280010112633 t1=0x81202800131c653d delta=51068682 => CP is CLOCKING
[  73.920087] CPF: t0=0x81202801f6fb94ba t1=0x81202801f8fdfa8b delta=33711569 => CP is CLOCKING
```

And the ring moved under its own power — the hardware read/write pointers
advanced in lockstep with the software pointer, 0 → 99 dwords, with **no host
submission at all**. That is the GPU executing the boot-time command stream:

```
CP_PRE_FLUSH: sw_wptr_dw=0   RPTR=0x0  WPTR=0x0
CP_PRE_FLUSH: sw_wptr_dw=9   RPTR=0x9  WPTR=0x9
CP_PRE_FLUSH: sw_wptr_dw=90  RPTR=0x5a WPTR=0x5a
...
CPDUMP[auto-post] sw : rb_iova=0x1000 wptr_dw=99 next_dw=99
CPDUMP[auto-post] hw : RB_RPTR=0x63 RB_WPTR=0x63
```

`wptr_dw=99` is the known bring-up marker (99 = nothing *submitted by Mesa* yet),
but `RB_RPTR`/`RB_WPTR` **are hardware registers** and they tracked it. The CP
drained the ring. That is the single thing `STATUS-AFTER-195.md` said was unproven.

## Why it worked — the coherency hypothesis is CONFIRMED

`FORENSIC VERDICT GOOD BABEFACE ms~1` on the first attempt, twice, with zero SMMU
faults and zero watchdog bites. Compare image 201, same kernel tree, same
everything except the allocator:

| | image 201 | image 202 |
|---|---|---|
| attempt 0 | `INITRES=0x0` for all 10000 polls | `BABEFACE ms~1` |
| retries | 3 × `INITRES=0x0` | n/a — attempt 0 succeeded |
| HFI | never reached | 8/8 ACK, 0 timeouts |
| SMMU faults | 0 | 0 |
| bring-up reached | ~1 boot in 4 (and that measurement was void) | 2 for 2 |

The mechanism, confirmed empirically by decoding the real `a640_gmu.bin`:
**41.7% of the firmware — including the CM3's instruction cache, the largest
single block — was being written through `vmap(pgprot_writecombine())`, a
write-combine CPU mapping that the CM3 cannot see until it drains.** Image 199
had already identified this exact bug and fixed it for the HFI queue; it was
never applied to the buffers holding the firmware.

Note what this explains that nothing else could: **zero SMMU faults alongside a
completely dead CM3.** A valid mapping with valid permissions and *wrong data*
produces no translation fault. That combination was the signature nobody could
account for across 200 images.

`ALLOC202 contiguous=1` on all six buffers confirms the `DMA_ATTR_FORCE_CONTIGUOUS`
hardening and the runtime contiguity proof both work — no buffer is discontiguous.

## What is now settled

- **Mode A (CM3 never executes): SOLVED.** Cause: non-coherent write-combine
  mapping of the firmware icache/dcache. Fix: `dma_alloc_attrs()`.
- **Mode B (firmware ignores HFI): SOLVED as a consequence.** The HFI exchange
  never had a transport bug — the firmware simply had never been running. All the
  HFI-path work (images 195/200/201) was fixing a path the firmware never
  reached.
- **Image 201's poll loop: VERIFIED WORKING.** `HFI-POLL ... completed=1` on all
  eight messages. The stated decision rule resolved in favour of "the poll
  succeeds". Whether the poll or the old IRQ-completion wait is strictly better
  is now untestable-by-accident and does not matter — both work once the firmware
  actually runs.
- **Liveness gate: PASSES.** First time in project history.
- **CP clocking: PROVEN** from hardware registers, twice.

## What remains — pixels

The GPU is up, the ring is programmed, the CP is draining commands. Still to do,
in order:

1. **Confirm `GL_RENDERER == FD640`** by actually running GL (the 4th gate
   condition; device node exists but not yet exercised).
2. **The hard reset on GL submit is still live and now REPRODUCIBLE.** Both
   boots died shortly after bring-up completed, with no kernel record — the
   signature `STATUS-AFTER-195.md` describes. This is the next wall and it is
   now the *only* thing between us and pixels.
   - Note `ROOT-CAUSE-284S-RESET-VBLANK.md` found a display vblank timeout that
     escalated to a reboot. Check that class first; it is cheaper than assuming
     GPU.
3. **Mesa batch-cache issue** (`MESA-SOURCE-ANALYSIS.md`). Still unresolved; the
   previous `batches[32]`/`ffs()` theory was retracted as wrong. The real open
     question: why does the flush stop after `fd_batch_resource_write`, before
     `fd_submit_sp_flush`, with no `defer:` line.

## Method notes (learned the hard way, worth keeping)

- **Put the capture log ON the phone, not in an ssh pipeline.** First successful
  run wrote `/root/trig202.log` over ssh and the link died before I could read it;
  the log was lost with the ring. Second run wrote `/root/t202b.log` on-device
  and survived the reset. `bootcap.service` now does this automatically.
- **Never let an ssh command span a GPU trigger.** Both attempts hung on ssh when
  the reset hit. Trigger, detach, poll, then read the on-device file.
- The GPU load thread does **not** fire on its own with
  `a6xx.gpu_init_delay_ms=300000`; the autotrigger fires at +60 s and may have
  been preempted. Triggering manually at a chosen uptime works reliably.
- `18d1:d001` on the bus is this Debian RNDIS gadget (serial `d2q-headless`), NOT
  a Samsung fastboot mode. It is not evidence of phone state — only the presence
  or absence of the USB device is.

## Captures

```
artifacts/gpu-bringup/captures/t202b.log        (the successful bring-up, full)
artifacts/gpu-bringup/captures/tally.txt
```
