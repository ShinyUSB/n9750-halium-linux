# Image 108 — HFI response decoded: valid ACK, wrong dispatch field

## Decode (q1[0], rd=0)

```
header = 0x00010401 -> id=1 type=1 size=4 seq=0
ret_header = 0x00100201 (echoes OUR request: id=1 FW_VERSION, seq=1, CMD)
error = 0x00000000
payload = 0x20000105 (firmware version)
```

## A–E answers

- **A)** Valid header, size 4 ≤ 19. Not oversized. (Image-107's BUG hit a
  different/later message in a 13-dword backlog; this run has exactly one
  4-dword message.)
- **B)** Correct offset: rd=0, message starts at ring base, parses coherently
  (request echo + error + version payload in order).
- **C)** It IS the FW_VERSION ACK in substance (type 1 = `HFI_MSG_ACK`,
  echoes request, error 0) — but our tasklet dispatches on
  `HFI_HEADER_ID == HFI_F2H_MSG_ACK (126)` instead of type, so it is
  silently ignored and the waiter times out after 5 s.
- **D)** Not malformed in any field.
- **E)** Units consistent (dword indices throughout; span=4 matches size).

## FW_VERSION reply

Payload `0x20000105`. Our code never validates the version number (v1 path
doesn't either); the failure is purely that the ACK is never matched to its
waiter.

## Standing root cause (not patched)

`a6xx_hfi_task` must dispatch v2 ACKs by message TYPE (`== HFI_MSG_ACK`),
not by ID `== 126`. One-line-class fix, withheld per instructions. BUG_ON
left intact.

Image 108 `31c195eb2d7b3a802fedeb488238dfe8f47ba620fa1acf5271ba6caf8d8d6760`.
SDE 0, DT +0, production untouched.