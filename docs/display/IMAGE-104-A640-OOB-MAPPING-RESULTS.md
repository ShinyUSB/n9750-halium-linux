# Image 104 — A640 OOB mapping: no change (B)

## Masks verified live

`SET_MASK=0x40000000 ACK_MASK=0x80000000 CLEAR_MASK=0x80000000`
(BIT30 / BIT31 / BIT31 — vendor A640 mapping active.)

## Outcome

```
oob BOOT_SLUMBER ack-wait enter
oob BOOT_SLUMBER ack-wait TIMEOUT (INTR_INFO=0x0)
gfx_rail_on returned ret=-110
fw_start end ret=-110
```

ACK still absent; `INTR_INFO` reads `0x0` — the GMU sets no ack bit at all,
old or new position. Gate held; device alive; no SMMU fault.

## Interpretation: B

New masks change nothing. The GMU does not acknowledge BOOT_SLUMBER under
either mapping, so the OOB bit positions are ruled out as the blocker. No
alternative bit combinations attempted in this image.

Image 104 `897910097b2c64260f92cedfa34284eca5ff27171dcb9883ad24445519dd97ba`.
SDE 0, DT +0, production untouched. Stopping.