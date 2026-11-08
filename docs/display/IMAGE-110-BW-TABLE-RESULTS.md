# Image 110 — canonical BW table: still timeout (outcome 2)

## Table verified live (exact canonical values)

```
size=160 bw_levels=1 cnoc_num=3 ddr_num=3 cnoc_wait=0x1 ddr_wait=0x1
ddr[0..2] addrs 0x50000/0x5003c/0x5000c, data 0x40000000
cnoc[0..2] addrs 0x50034/0x5007c/0x5004c
```

Header (computed): `(3<<20)|(0<<16)|(160<<8)|3` = `0x0030A003`.

## Outcome

`BW_TABLE id 3 timed out waiting for response` → `ret=-110`.
Preserved canonical table in image as instructed.

## Distinguishing evidence

- `kgsl_hfi_irq` count = **3** (was 1 after the version-only cycle):
  version ACK + perf ACK + one more — response path demonstrably works.
- `kgsl_gmu_irq` = 0 throughout.
- No SMMU fault, no panic; device stable under gate.
- q0/q1 indices + INTR bits at BW timeout: not captured (no instrumentation
  on the BW path beyond send/recv markers; stream filter excluded SNAP
  workers which only arm for FW_VERSION).

## Reading

GMU answers version + perf but not BW — consistent with payload-level
rejection (firmware drops the table without ack) rather than transport
failure (IRQs demonstrably arrive) or firmware-state stall. The remaining
unknown is what about the table the firmware rejects: values (verified
canonical), size (160 dwords — matches struct), or queue addressing.

Image 110 `1cae6421488a97c83c14d8f12dcfa5abafe6ce0ef07c8e28f9789f2f8aa8db83`.
SDE 0, DT +0, production untouched. Stopping.