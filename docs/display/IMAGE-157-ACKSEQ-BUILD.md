# Image 157 (build-only) — seqnum-only ACK dispatch (vendor parity)

## Why
Live run showed vote answered with `ret_id=239 ret_seq=6 error=0`
while waiter wanted id=30: dropped → `-110`. Vendor `receive_ack_cmd`
matches SEQNUM ONLY. Our extra id check converts firmware answers into
timeouts. Prior vote-timeouts may include such mismatches (unverifiable
retroactively — HFI-ACK lines logged only id/seq/error).

## Diff (`a6xx_hfi.c` dispatch only)
- Match `resp->seqnum == seqnum`; log `HFI-ACK idmismatch` when ids
  differ (diagnostic gold for future runs). Sequential sends keep seqnum
  unique in practice. Error propagation unchanged.
- Untouched: everything else.

## Checks
- fast: 0 errors, 0 warnings. full: EXIT 0, 0 errors.
- Packaged with 145 cmdline (all flags + skip_bw, no hold).
- Image 157 `157-halium-boot-a6xx-ackseq.img`
  sha256 `2430df46a6b2e9f7c63e3bc066475cc8b8a40b0ddfeae9df5b6ef3f868a119ab`.

No flash performed; stopping.
