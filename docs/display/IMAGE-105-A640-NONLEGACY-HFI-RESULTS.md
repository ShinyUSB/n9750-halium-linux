# Image 105 — non-legacy path: HFI STARTS (A)

## Chain (first-ever complete GMU boot on DRM)

```
legacy=0
skipping gfx_rail_on (non-legacy A640)
sptprac poll TIMEOUT (status=0xc0)   <- non-fatal by design (returns 0)
entering hfi_start
hfi poll ok (STATUS=0x1)
hfi_start returned ret=0
fw_start end ret=0
```

~5 s later, from frequency scaling (not the boot path):
```
oob GPU_DCVS ack-wait TIMEOUT (INTR_INFO=0x0)
```
Device alive, zero panics/oops. Gate never tripped (no error to gate on).

## Interpretation: A

Skipping the legacy `gfx_rail_on()` OOB unblocked HFI start. The GMU boot
completes; HFI handshakes. The control-flow mismatch (not any register value)
was the blocker since image 102's first boot.

The follow-on DCVS OOB timeout is a separate, later, non-fatal path
(frequency scaling talks OOB the same way). Not touched in this image.

Image 105 `18ef8b5af0265bdfce83fb4582f8357ca07a37fb7cf5a0070428c311d75b5a85`.
SDE 0, DT +0, production untouched. Stopping as instructed — no hw_init/submit
probing in this image.