# Image 91 — v5.8 layout: no change in fault

## Layout (verified live)

```
LAYOUT dummy=0x60000000/0x1000 hfi=0x60001000/0x4000
LAYOUT icache=0x4000/0x3c000 dcache=0x44000/0x3c000
RB QTBL_ADDR=0x60001000 (expect 0x60001000)   <- readback matches new HFI IOVA
```

## Runtime result

```
QTBL readback -> +~1.5 ms ->
Unhandled context fault: iova=0xdeadbee0, cb=1,
  fsr=0x40000402, fsynr0=0x60021 (truncated in capture)
Kernel panic: No Device SMMU Fault - SID=0x405
```

Identical IOVA/CB/SID/FSR to images 86/88/90. Nothing moved into
`0x60000000` or HFI/dummy regions.

## §8/§9 verdict

The fault is byte-identical under both layouts, so dummy/HFI placement is
neither root cause nor effect modifier for this fault. Per the interpretation
rule, no claim beyond: the Samsung firmware's first-millisecond DMA does not
depend on the v5.8 dummy/HFI arrangement.

Image 91 `32855f2298d9a5c1cc5327a25ff7bee9c5d1cf0af164ba465d7c73c1a3cd188b`.
SDE 0, DT +0, production untouched. No further patch applied.