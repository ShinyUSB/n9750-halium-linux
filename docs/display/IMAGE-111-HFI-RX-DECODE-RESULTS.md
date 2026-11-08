# Image 111 — HFI RX decode: valid ACK consumed, then unknown message, then BUG

## Ring walk (reconstructed)

| read | rd range | content | disposition |
|---|---|---|---|
| 1 | [0..3] | FW_VERSION ACK (`00010401`, echo `00100201`, err 0, ver `20000105`) | matched waiter ✓ |
| 2 | [4..6] | `001103c0 0d9ea9c0 0000006f` → id=192, type=1, size=3, seq=1; ret echo id=192/seq=217, error=111 | type==1 → treated as ACK; no waiter matches (ids 192/217 never sent) — silently ignored |
| 3 | [7..] | unknown (BUG_ON fired inside `queue_read` before any dump) | panic → reboot |

## A–E

- **A)** First header valid (size 4 ≤ 19). The BUG message content was never
  captured (BUG fires before any dump by design this round).
- **B)** rd=7 IS a message boundary (4+3 consumed). Plausible start; content
  unknown.
- **C)** The [4..6] message is ack-*shaped* (type 1) but references id/seq
  never sent — async event or firmware-side message this host doesn't model.
  No ERROR message (id 100) was seen.
- **D)** [4..6] parses cleanly; not malformed. The faulting message is
  unobserved.
- **E)** Dword units consistent throughout (span arithmetic matches).
- **FW_VERSION reply:** valid, consumed, waiter completed (same as image 109).

## Standing

Tasklet consumes in order and dies on the first oversized/unknown message.
To identify message [7..], the dump must move *before* the BUG_ON (inside
`queue_read`, pre-check) — proposed as the single next instrumentation
delta, not applied here.

Image 111 `821869f62ec80c72d1831d6210229d80f4a4c8b329b96e33450505ad50d7b74a`.
SDE 0, DT +0, production untouched. No fix applied.