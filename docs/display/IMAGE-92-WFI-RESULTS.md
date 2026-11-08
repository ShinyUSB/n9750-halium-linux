# Image 92 — WFI_CONFIG: no change in fault

## Write verified

```
before WFI_CONFIG write (off=0x5002 value=0x0)
after WFI_CONFIG write, readback=0x0
```

Register accepted the write (readback matches).

## Fault (identical to image 91)

```
before GMU start:                 42.127896
+1.6 ms:
Unhandled context fault: iova=0xdeadbee0, cb=1,
  fsr=0x40000402, fsynr0=0x60021 (truncated)
Kernel panic: No Device SMMU Fault - SID=0x405
```

| | image 91 | image 92 |
|---|---|---|
| IOVA | 0xdeadbee0 | 0xdeadbee0 |
| CB | 1 | 1 |
| FSR | 0x40000402 | 0x40000402 |
| SID | 0x405 | 0x405 |
| timing | ~1.5 ms post-start | ~1.6 ms post-start |

## Interpretation per instructions

No behavioral change. WFI_CONFIG is ruled out as a factor. HFI changes
explicitly deferred. Stopping here.

Image 92 `edc9d07436ef147fbc5bec5a88a8c10b4db2ae71b166c544fbd9b51a6c9c8127`.
SDE 0, DT +0, production untouched.