# GMU snapshot results (image 90, SMMU-IRQ hook)

## Fault (unchanged signature)

```
FW_START state=1 (cold)
PRE_PDCSEQ → rpmh_init → fw_load → ... → gmu_start
+~1.5 ms: iova=0xdeadbee0, cb=1, fsr=0x40000402 (TF), fsynr0=0x60021 (READ)
panic: No Device SMMU Fault - SID=0x405
SMMU ttbr0=0x20003e5e3d000 ttbr1=0x20003e312e000 sctlr=0xc000e7
```

## Host registers at fault (re-read live)

`QTBL=0x60000000 INFO=0x1 SFR=0x6040100 BOOT=0x2 CM3CFG=0x4052 G7=0x1`
(all exactly as programmed; HFI table head `00000000 00000078 00000006
0000000c` — sane empty table). Full INITRES/HFISTAT tail cut off in capture;
to be re-pulled if device returns.

## TCM at fault vs static image (programmatic word compare)

| range | verdict |
|---|---|
| ITCM +0x20..0x2C (`43f80 deadbeef 00000200 e000ed08`) | SAME — aperture decode proven correct |
| ITCM +0x00..0x1C (reset stub) | CHANGED |
| ITCM +0x30..0x7C (unloaded by host) | CHANGED (nonzero — firmware wrote it) |

The GMU executed: reset stub ran, main firmware ran its copy/relocation loop
(`bl #0x408` prologue disassembled), ITCM low regions rewritten by the
firmware itself. This is normal bootloader self-relocation, NOT corruption.

## Answers (§16)

- **ITCM+0x24 at fault time: still `DEADBEEF`.** Never modified by anyone.
- **Registers FW_START→fault:** host-written values all stable (table above);
  no host register changed.
- **PSP-path evidence:** disassembly proves the stub installs PSP=`0xDEADBEEF`
  (`msr psp, r0`); the fault reads `0xDEADBEE(x)`; no other DEADBEEF-derived
  address exists in image or host state; firmware demonstrably executed into
  the phase where PSP gets used.

## Distinction (§17): firmware-internal, not missing host init

The host owns nothing the firmware consumes as PSP — the poison is installed
by firmware code from its own image, unpatched by KGSL either. The remaining
question is *what triggers PSP use here but not under KGSL* (exception return
after a silent primary event vs thread-mode switch in main flow). That needs
GMU-internal tracing (ETM) or HFI-table comparison against KGSL's
initialization — both beyond host markers.

No fix applied. SDE 0, DT +0, production untouched. Image 90
`8b885b387c2578df6a04c13ff8df8a3fd4d900ef5afb87364c5d47428a9186cf`.