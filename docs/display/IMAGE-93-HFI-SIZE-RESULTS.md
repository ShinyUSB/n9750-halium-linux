# Image 93 — HFI size units: no change in fault

## Live verification

`RB hfi[0..3] = 00000000 0000001e 00000006 0000000c`
(size `0x78` → `0x1e`; qhdr0/qhdr_size/queues unchanged; queue addresses and
contents untouched.)

## Fault (byte-identical to image 92)

```
Unhandled context fault: iova=0xdeadbee0, cb=1,
  fsr=0x40000402, fsynr0=0x7002
Kernel panic: No Device SMMU Fault - SID=0x405
```

## Verdict

Size-units hypothesis **ruled out**. No further change in this image.
Image 93 `c10aa5181ee2d369239ef070c2cd3d0ab268650854e3cdd61e06af61ab134b70`.
SDE 0, DT +0, production untouched.