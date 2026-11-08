# ROOT CAUSE (superseded in one respect) — the GMU firmware never answers the HFI fw_version query

Kernel `#138` (image 194). This record corrects the previous one.

## Correction to my previous conclusion

I concluded on image 193 that the cause was `a6xx_gmu_hfi_vote_probe()` failing
and triggering a retry. **The vote probe was not the cause.** Image 194 fixed
that (the probe can no longer fail bring-up) and the same suppression still
occurred:

```
POSTINIT[00] ENTER_HW_INIT_WRAPPER needs_hw_init=1
A6XX_DIAG: hw_init suppressed after GMU boot error -110; staying alive
```

The 194 change was therefore **ineffective** — it fixed a real fragility but not
the actual failure. I should not have presented it as "the fix".

## What image 194 actually shows

```
[79.779899] A6XX_TRACE: after GMU start (ret=0)      <- GMU boots fine
[79.890027] POST_START: sptprac poll TIMEOUT         <- non-fatal (returns 0)
[79.890101] POST_START: hfi_start returned ret=0     <- HFI init OK
[79.890111] POST_START: fw_start end ret=0
[84.960147] HFI-E fw_version timeout                 <- 5s later, NO answer
[84.960241] A6XX_TRACE: hfi_start returned (ret=-110)
[84.960246] RETRY142: attempt 1/2 after ret=-110     <- re-boot attempted
[86.005177] A6XX_TRACE: after GMU start (ret=-110)   <- retry fails too
[87.069821] A6XX_DIAG: hw_init suppressed after GMU boot error -110
```

The decisive line (`a6xx_hfi.c:399`, HFI-T):

```
HFI-T id=1 seq=1 q0 rd=0 wr=2 q1 rd=0 wr=0
       hfi_irqs=0(+0) gmu_irqs=0(+0) INTR_INFO=0x0 MASK=0xff7ffffe H2G_RAW=0x0
HFI-E q1resp: 00000000 x16        (response queue entirely empty)
```

## Root cause

**The GMU firmware does not process the HFI fw_version request and never raises
the HFI response interrupt.** We wrote the command (`q0 rd=0 wr=2` — the write
pointer advanced, so the host side did its job), but:

- `hfi_irqs=0(+0)` — the HFI IRQ handler (`a6xx_gmu.c:117`) ran **zero** times
- `gmu_irqs=0(+0)` — the GMU IRQ ran zero times
- `H2G_RAW=0x0` — no host→GMU interrupt pending or acknowledged
- `INTR_INFO=0x0` — the GMU never signalled anything back
- `q1` response queue all zeros — no reply was ever written

The interrupt mask is **correct**: `0xff7ffffe` leaves bit 0 (`MSGQ`) unmasked,
which is the HFI response bit (`A6XX_GMU_GMU2HOST_INTR_INFO_MSGQ = 0x1`). So
this is not a masking bug — the firmware simply never responded.

This also explains the earlier `INITRES=0xbabeface` confusion: the firmware
*does* self-init (that is CAF's success code and our poll waits for it). It then
stops responding to host HFI messages.

Net effect, unchanged from before: the resume returns `-110`, that latches
`a6xx_gmu_boot_error`, `msm_gpu_hw_init()` is skipped, the ring is never
programmed (`rb_iova=0x0`), and `wptr_dw` stays 0 while `GL_RENDERER` reads
`FD640` and `GEM_SUBMIT` returns 0.

## Why this is a genuinely different (deeper) problem

The firmware runs from ITCM and self-init completes, but it does not service
the HFI message queue. That is consistent with the GMU being unable to read
memory it needs — which is the same class of problem as the image-192 ITCM hole,
one level deeper. Image 192 removed the *host-visible* SMMU faults (5 → 0); it
did not necessarily make the firmware's own memory access work.

I have **not** proven that link. It is a hypothesis worth testing, not a
conclusion.

## State of the fixes

| image | change | status |
|---|---|---|
| 190 | DRM version 4 → `sp_funcs` | verified good |
| 191 | `MSM_SUBMIT_BO_DUMP` | verified good — `GEM_SUBMIT=0` |
| 192 | ITCM hole at IOVA 0 | verified good — SMMU faults 5→0, GPU life 2.5s→295s |
| 193 | vblank WARN → `pr_info` | verified good — no reboot at ~284s |
| 194 | vote probe can't fail bring-up | correct in principle, but **did not fix this** |

None of them are wrong; 194 simply was not the bug.

## Next step

Stop patching the bring-up result and instrument the **HFI request itself**:

1. Confirm the fw_version message reaches the GMU: compare the bytes we write
   against CAF's `HFI_H2F_MSG_FW_VERSION` encoding in
   `drivers/gpu/msm/adreno_a6xx_hfi.c` (CAF has its own implementation — diff it).
2. Check whether the GMU's HFI queues need an explicit start/doorbell poke that
   CAF does and we do not.
3. Revisit whether the ITCM/DTCM firmware memory is genuinely accessible — the
   "firmware can't read memory" hypothesis would explain a silent firmware.

The CAF `adreno_a6xx_hfi.c` in-tree is the authority for #1 and #2 and has not
been diffed yet.