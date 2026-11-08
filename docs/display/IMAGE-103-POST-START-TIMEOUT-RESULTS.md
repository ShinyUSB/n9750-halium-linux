# Image 103 — first failing stage: OOB BOOT_SLUMBER ack timeout in gfx_rail_on

## Sequence (outcome A)

```
POST_START: fw_start begin
POST_START: enter gfx_rail_on
POST_START: oob BOOT_SLUMBER ack-wait enter
POST_START: oob BOOT_SLUMBER ack-wait TIMEOUT (INTR_INFO=0x0)
POST_START: gfx_rail_on returned ret=-110
POST_START: fw_start end ret=-110
```

sptprac/hfi_start never ran (control flow preserved). Device alive at 101 s,
zero panics/oops — gate held.

## Reading

`HOST2GMU_INTR_SET[BOOT_SLUMBER_REQUEST]` is written, but
`GMU2HOST_INTR_INFO[BOOT_SLUMBER_ACK]` never sets within 10 ms. The GMU —
which demonstrably executes (memtest patterns, descriptor tables, BADDFACE
verdict) — does not answer the OOB doorbell. Candidate causes (not tested):
OOB interrupt routing/mask on this path, GMU waiting on an unmet precondition
(HFI handshake never started on our side), or firmware expecting ACD/other
state first. Notably our `a6xx_gmu_irq_enable` runs before this point and the
ack is *polled*, not interrupt-driven, so host IRQ handling is not in the loop
— the GMU itself withholds the ack.

Image 103 `f9348e7feb27fa5f73919194480a88c1484b46c56a66a0a22b3b921fd05f886b`.
SDE 0, DT +0, production untouched. Markers only; stopping.