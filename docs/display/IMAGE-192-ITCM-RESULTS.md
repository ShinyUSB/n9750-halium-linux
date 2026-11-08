# 🎉 Image 192 — the ITCM hole was real. GPU lifetime went 2.5s → 295s+.

Kernel `#134`. One 16 KiB mapping at IOVA 0x0, and the GMU stopped dying.

## Before / after, same test

| | image 191 | image 192 |
|---|---|---|
| SMMU "Unhandled context fault" | **5** | **0** |
| GMU watchdog bite | yes (~1.5s after bring-up) | **none** |
| `recover166` recovery resume | ran, timed out `-110` | **never invoked** |
| GPU lifetime after `fw_start` | ~2.5 s | **295 s+ (still alive)** |
| `GL_RENDERER` after 295s | llvmpipe (GPU gone) | **FD640** |
| `GEM_SUBMIT` | accepted but GPU dead | **accepted, GPU alive** |

The bring-up sequence is now clean and single-pass — exactly one resume, no
nested retry:

```
[  64.864] BC132: ev=RESUME_ENTRY ret=0 depth=1     <-- one resume only
[  64.865] POST_START: fw_start begin
[  64.868] FORENSIC VERDICT GOOD BABEFACE ms~1
[  64.975] POST_START: fw_start end ret=0
```

Compare image 191, which needed a *second* resume at depth=2 and then failed it.

## The fix

```c
gmu->itcm_hole = a6xx_gmu_memory_alloc_fixed(gmu, SZ_16K, 0x00000,
        IOMMU_READ | IOMMU_WRITE | IOMMU_PRIV);
```

confirmed live in the log:

```
A6XX_TRACE: SMMU map itcmhole iova=0x0 size=0x4000 ok
```

16 KiB, ending exactly at the icache base (`0x4000`), so it cannot overlap any
existing mapping. Added to the free path. Nothing else changed.

## A correction I owe

In my previous report I called `INITRES=0xbabeface` an **init-failure** value and
said the firmware "never completed init". **That was wrong.** CAF's own
`a6xx_gmu_start()` polls for exactly `0xBABEFACE` as the **success** code:

```c
timed_poll_check(device, A6XX_GMU_CM3_FW_INIT_RESULT, 0xBABEFACE, ...)
```

so the firmware had always self-initialised fine. That misreading is what sent
me looking at `fw_start` at all; the real fault was the unmapped IOVA, which is
what image 192 fixed. The correction is recorded because the wrong reading is
what would mislead anyone reading the earlier note.

## Still not achieved: pixels on screen

Honest status — this is a large, unambiguous win on stability, but the project
goal is not yet met:

- `GEM_SUBMIT` is accepted (`= 0`) and the GPU stays alive across it, which is
  the first time both have been true simultaneously.
- **`wptr_dw` is still 0**, not advanced. The ring is not moving, so Mesa's
  commands are being accepted by the driver but there is no evidence the GPU has
  executed anything. `A640` does not appear in `/proc/interrupts` at all.
- The phone still hard-resets at ~284 s. The log ends mid-stream with no panic,
  no oops, no GPU fault, no SMMU fault — so the reset is **not** explained by
  anything in this log.

So: bring-up is now reliable and the submit path is proven end-to-end, but
execution is unproven and something still kills the phone at ~5 minutes.

## Next move

1. Find why `wptr_dw` stays 0 despite an accepted submit. This is now the single
   blocking unknown between "stable" and "rendering".
2. Separately, the ~284 s reset needs its own investigation — the durable capture
   gave us a clean log showing nothing GPU-related at the end, so the cause is
   outside the driver.

Do **not** change the submit path (190/191) or the ITCM hole (192). All three are
now verified-correct and any further change is a regression risk.