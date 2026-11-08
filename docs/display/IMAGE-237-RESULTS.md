# Image 237 — RB_NC_MODE_CNTL amsbc: applied, no change

Image: `artifacts/flash/237-halium-boot-a6xx-amsbc.img`
sha256 `07a6b4c8c8015d5f806d12cd48305dff3c4eff2702219d4faeacfb456f177fa6`
Kernel `#44`, boot `5aef206a`.

## Change (single variable from 235)

`REG_A6XX_RB_NC_MODE_CNTL`: `0x04` -> `0x14` (amsbc bit set).

Provenance, fully derived: DT `sm8150-gpu.dtsi:125` sets `qcom,ubwc-mode = <3>`
(UBWC 3.0); CAF's switch (vendor `adreno_a6xx.c:898-901`) gives mode=0,
amsbc=1 for UBWC 3.0, with the comment "Only valid for A640 and A680".
With `min-access-length = 32` (mal=0, `:123`) and `highest-bank-bit = 15`
(bit=(15-13)&3=2, `:121`), CAF writes `(1<<4)|(0<<3)|(2<<1)|0 = 0x14`.
TPL1/SP keep `0x04` (CAF agrees). TPL1/SP untouched.

## Bring-up clean

```
FORENSIC VERDICT GOOD BABEFACE ms~1
HWINIT[25] return ret=0
```

Trigger fired at ~346 s (past the 300 s deferral). No freeze.

## Result: no change

`dmabuf.py` (sound CPU-side oracle):

```
pixel[0,0]     = 0x00000000  (r=0 g=0 b=0 a=0)
nonzero pixels = 0 / 4096
VERDICT: GPU DID NOT WRITE THE SURFACE
```

**The amsbc hypothesis is refuted as the cause.** This fits the pre-existing
evidence that `noubwc` linear writes also never land (amsbc governs UBWC
addressing, so a negative result here is consistent, not surprising).

## Standing

| image | hypothesis | result |
|---|---|---|
| 234 | IOVA base off zero (fence hang) | FIXED the hang |
| 235 | A640 clock values | applied, no render change |
| 236 | LLC_NWA + NOEXEC map flags | HARMFUL, reverted (broke SQE fetch, `hw_init ret=-22`) |
| 237 | RB amsbc | applied, no render change |

Still untested, all CAF-verified:
- `CP_ME_INIT` mask `0x2f` vs CAF `0x16f` + `pwrup_reglist`
- GMU AO CGC registers (defines present, no writer)
- LLCC "gpu" slice + `GBIF_SCACHE_CNTL1` SCID (rail-timing-sensitive; the SMMU
  attr half was refused with -EBUSY, so the ordering problem stands)
- `CP_ROQ_THRESHOLDS_2` / `PC_DBG_ECO_CNTL` a630 values

Established facts that any theory must fit:
- Mesa emits a dense, valid PM4 stream; relocs patch to real IOVAs
  (e.g. RB blit dest `0x700025000`); the same buffer sampled post-execution
  across three submits stays zero.
- CP executes the stream (event-writes land, RPTR advances, fence retires).
- Address translation is real (arm-smmu, refuted 5 ways).
- No faults of any kind: no RBBM interrupts, no SMMU stall, no CP fault.
- `dmabuf.py` reads CPU memory post-swap: zeros are a sound finding.
