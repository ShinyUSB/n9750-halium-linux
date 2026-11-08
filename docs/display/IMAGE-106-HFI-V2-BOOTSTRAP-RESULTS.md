# Image 106 — HFI-v2 bootstrap: first message times out (B)

## Sequence

```
legacy=0, gfx_rail skipped, sptprac TIMEOUT (non-fatal)
hfi handshake ok (STATUS=0x1), fw_start ret=0
HFI: verify_fw_version enter
Message HFI_H2F_MSG_FW_VERSION id 1 timed out waiting for response
HFI: verify_fw_version ret=-110      <- STOPPED here, later messages unsent
```

## Record (B)

- command: `FW_VERSION` (id 1), seqnum 1, queue 0 (command queue)
- wait: 5 s completion timeout; no ACK
- queue read/write indexes at timeout: **not captured** (no live interface;
  buffer contents only in POST dumps, which run on BADDFACE path)
- GMU2HOST intr state at timeout: not captured for the same reason
- ACK contents: none (timeout, zero responses)
- IRQs: `kgsl_hfi_irq` fired **once** total; `kgsl_gmu_irq` **zero**
- device alive, zero panics (gate held)

## Reading

The GMU booted (BABEFACE) but never answers the first HFI command. Either it
does not process the command queue (doorbell/queue-address problem), or its
response never arrives (IRQ/tasklet path). The single HFI IRQ firing is
consistent with the CTRL handshake only. Next single question: whether the
GMU sees the queued message — needs queue-index/IRQ-state capture around the
first send.

Image 106 `ca8810338778f7dcc6aa1b3f874d98541edf1f0dbafced61b4db0bf6ddc785d0`.
SDE 0, DT +0, production untouched. Stopping.