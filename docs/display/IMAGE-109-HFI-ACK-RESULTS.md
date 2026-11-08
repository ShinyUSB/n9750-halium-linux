# Image 109 — ACK dispatch fixed; bootstrap advances two steps, stops at BW

## Sequence

```
HFI-ACK id=1 seq=0 error=0  ->  verify_fw_version ret=0   (waiter completed)
HFI-ACK id=4 seq=1 error=0  ->  send_dcvstbl ret=0        (PERF accepted)
HFI: send_bwtbl enter -> ret=-110                        (BW TIMED OUT)
```

Waiter matching works (both ACKs consumed by their senders). No panic, no
SMMU fault; device stable under the gate.

## Standing

- ACK dispatch fix confirmed functional (2/2 ACKs matched).
- Next first failure: `HFI_H2F_MSG_BW_TABLE` (id 3) — no response in 5 s.
- The BW message is the largest bootstrap payload (CNOC/DDR command tables
  with addresses/data arrays). Untouched next question: whether it is size,
  content (vote addresses), or firmware state that blocks the ack.
- No fix for BW in this image.

Image 109 `f2f5e5e720a3eba89ea4227e66f382a91cd34931af71fa55b0eb90e5c3b59e56`.
SDE 0, DT +0, production untouched. Stopping as instructed.