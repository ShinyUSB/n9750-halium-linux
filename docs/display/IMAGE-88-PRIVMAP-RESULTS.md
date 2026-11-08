# Image 88 — permission fault GONE; new fault is a poison-address read

## Exact new fault (last_kmsg)

```
before GMU start:                 36.318618
+1.54 ms:
Unhandled context fault: iova=0xdeadbee0, cb=1,
  fsr=0x40000402, fsynr0=0x60021, fsynr1=0x7
Kernel panic: No Device SMMU Fault - SID=0x405
```

- IOVA `0xdeadbee0` = `0xDEADBEEF` masked to page — poison, unmapped anywhere.
- CB 1 (cbndx; gmu domain's CB).
- SID `0x405` (unattached master in CB1's SMR group).
- FSR `0x402` = translation fault (back to TF class).
- FSYNR0 bit 4 clear = READ.

## What this establishes

1. The `0x4dc0` permission fault is GONE — IOMMU_PRIV fixed exactly what it
   was supposed to. GMU privileged icache reads now translate.
2. The GMU now executes far enough to issue a NEW read to a poison address.
   `0xDEADBEEF` is the Samsung `a640_gmu.bin` magic (`deadbeef deadbeef` opens
   the blob). Something on the GMU side uses an uninitialized/magic value as a
   DMA address ~1.5 ms after start.
3. Static image placement is complete (all 7 blocks verified); this is a
   runtime address handoff, not image loading.

## Unchanged (per scope)

SDE 0, DT +0, IOMMU adapter `fac06af5…`, PDC routing, parser logic, power,
clocks, CM3_CFG, HFI, fences/submit, Mesa/Weston untouched. Production empty.
Image 88 `3fc04c97c91471a27b9cdf6b2c7b165769ae19823032da093b22c59c18d72330`.