# Image 112 — q1 message classification (device alive, no BUG this run)

## Ring walk (complete, self-consistent run)

| read | range | content | disposition |
|---|---|---|---|
| 1 | [0..3] | FW_VERSION ACK: id=1/type=1/size=4/seq=0, ret echo (1,CMD,2,1), err 0 | matched waiter ✓ |
| 2 | [4..6] | PERF ACK: id=4/type=1/size=3/seq=1, ret echo (4,CMD,59,2), err 0 | matched waiter ✓ |
| 3 | [7..] | zeros (wr=12, 5 dwords "pending" but unwritten) | clean empty read, no BUG |

Followed by: BW send → 5 s timeout → -110 (same as images 106/110).

## A–F classification

- The [4..6] message is a **valid ACK** (A): correct type, coherent ret echo
  of our PERF send (id 4, seq 2), error 0. Consumed normally.
- q1 rd=7 **is** a message boundary (B, confirmed: 4+3 consumed).
- No ERROR message, no malformed header this run (C/D negative).
- Units consistent (E).
- FW_VERSION reply re-confirmed (same as image 108).

## Variance note (honest)

Image 111's run showed `0x001103c0` (low byte 0xC0) where this run shows
`0x00110304` (low byte 0x04) — all other words identical. Either the GMU
posts varying low-byte flags, or one read caught a transitional value
(read⿿while-GMU-writes race on shared ring memory). Both runs agree the
message is the PERF ACK; only the low byte varied.

## Standing BUG assessment

The image-107 BUG fired on a *later, bigger* message in a 13–25 dword
backlog — never on these small ACKs. With the ACK dispatch fixed (img 109),
the tasklet consumes all valid traffic; the remaining BUG exposure is only
for genuinely oversized/unknown messages, none seen since.

Image 112 `7760460332ae1979ca87f6c8af0b78ffb1af8a10c48861168e2d7a52a8b73029`.
SDE 0, DT +0, production untouched. No fix applied.