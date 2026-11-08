# Image 101 — 4-queue HFI topology: no change

## Table verified live

```
hfi = 00000000 00000036 00000006 0000000c   (size 0x36, qhdr0 6, qhdr_size 0xc)
q0 status=1 type=0xa00  iova=0x60002000
q1 status=1 type=0xa01  iova=0x60003000
q2 status=1 type=0x2802 iova=0x60004000
q3 status=0 type=0x1403 iova=0x60005000
```

Exact vendor shape (HFI base is 0x60001000 in this layout, so data pages sit
at +0x1000…+0x4000 as specified).

## Outcome (interpretation B)

- `fw_start returned (ret=-110)` — same BADDFACE timeout.
- Gate held (`hw_init suppressed … staying alive`); device alive 140 s+.
- Zero SMMU faults, zero panics, zero oops this boot.

The 4-entry table hypothesis is ruled out. HFI topology does not determine
the boot verdict. No further change in this image.

Image 101 `def50e1e2dde75ce61ac104f62c0a2d0a40a840f8629fa0ac1b952d47f40e97e`.
SDE 0, DT +0, production untouched.