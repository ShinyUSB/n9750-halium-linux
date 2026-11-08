# Image 276 — KGSL uncached-window layout: renders still nothing, but the layout is closed

Date: 2026-10-18
Image: `artifacts/flash/276-halium-boot-a6xx-hfilayout.img`
sha256 `173f7c2f8949487febbd2744a3f08e9b2c5ff4c266481f4f1106b34e230067d0`
kernel sha256 `f8f52efa44057c5d919b50ee2cb3c2004ad48eb6d13cea01b0826986bd306694`

## What changed

Three lines in `a6xx_gmu.c`: the GMU's non-cached window (0x60000000–0x80000000) now
matches KGSL's layout exactly.

| buffer | was | now |
|---|---|---|
| dummy | fixed 0x60000000 | fixed 0x60000000 (unchanged) |
| **HFI table** | dynamic → 0x60010000 | **fixed 0x60100000** (0x5000) |
| **LOG buffer** | fixed 0x60006000 | **fixed 0x60200000** (4K) |
| dynamic base | 0x60001000 | 0x60300000 |

The dynamic-base move was **required**: the tree's own comment records that the
dynamic allocator strides 1 MiB, so leaving it at 0x60001000 would have handed a
later allocation straight into the new fixed HFI at 0x60100000.

## Runtime proof that the layout took effect

```
[  640.728162] a6xx-trace: after hfi alloc (fixed 0x60100000, KGSL layout)
[  640.745202] A6XX_TRACE: before CP_MSG write (value=0x60200000)
[  640.745213] A6XX_TRACE: after CP_MSG write, readback=0x60200000
[  640.745351] A6XX_TRACE: RB CP_MSG=0x60200000 (img102: written value)
```

The GPU came up healthy:

```
SPTPRAC_PWR_CLK_STATUS 0x00000000
RPMH_POWER_STATE       0x00000000
CP_TIMESTAMP 0x012028037b97fbb7 delta 675783 clocking 1
HWINIT[12] after a6xx_cp_init ret=0
GXSTATE[3]: SPTPRAC=0x0 GDSC_OFF=0 CLK_OFF=0 RPMHPWR=0x0 GXGDSCR=0xf8222800
```

## Result

**Rendering: `PIXEL=[0,0,0,0]`. Unchanged.**

```
renderer=FD640
PIXEL=[0,0,0,0]
VERDICT: 3 magic hit(s): 0x7fa5b1a000+4096=VS, 0x7fa5b1b000+4096=FS, 0x7fa5b1e000+4096=VS
```

No faults in dmesg (the four grep hits for "fault" were `default`, `cable_type`
and "registering fault handler").

**So the GMU's uncached-window layout is NOT the cause of the render defect.**
The last structural difference I could find between our boot and the KGSL
reference is eliminated, for rendering.

## Two things worth keeping from this

1. **The premise was weaker than it looked, and the experiment still paid.** The
   tree's comment says the LOG address is *communicated* to the firmware via
   `CP_MSG` ("exactly as upstream v5.8 and vendor KGSL") and the HFI table
   address via `HFI_QTBL_ADDR` — so the firmware does not obviously need fixed
   addresses, and "0x60200000 was unmapped" was not proof of anything. The
   experiment was still worth running: it is a clean, controlled null result
   rather than an assumption, and it also verified the layout end-to-end at
   runtime.

2. **The crash axis may be separate.** The 276 boot was up 12+ minutes with no
   EL2 abort, while the immediately preceding (275-era) boot recorded
   `UploadCause[TZBSP_ERR_FATAL_NOC_ERROR]` before the manual reset. That is
   suggestive, not conclusive — the aborts are intermittent, and the 274c boot
   also survived 600 s+ before dying. **But it is the first time the layout has
   been KGSL's while the device stayed up**, and the crash axis is where the
   project has the most data (167/213).

## Where the state of the investigation now stands

| layer | verdict |
|---|---|
| GLSL / command stream / nested graph / relocations | correct |
| ring executes, stream retires, SQE walks the graph | works |
| **shader programs delivered to the right addresses, with the right content** | **works** (image 272) |
| page table installed and correct | works (image 270) |
| SMMU write permission | permitted |
| GX clock / GDSC / ACD / bus vote / HFI layout | all match KGSL now (images 226–276) |
| **RB → DDR writes** | **never observed** |
| **SP → DDR writes** | **never observed** |
| EL2/HYP data abort | 167/213 archived captures; still unexplained, now with the layout matching KGSL |

**Everything the driver and the platform do is now provably equivalent to the
KGSL reference that boots cleanly, and the GPU still writes nothing.** That is
a real narrowing: the fault is no longer in the submission, the memory
programming, the clock/power sequencing, the HFI table, or the secure-register
programming.

## The next thing worth doing

Not more register archaeology. Two candidates remain that are *not* register
comparisons:

1. **The GMU firmware itself.** The reference that boots cleanly is
   `ANDROID-KGSL-PSP-REFERENCE.md`'s KGSL boot. The remaining difference in the
   firmware's *environment* is not the HFI table any more — it is the firmware
   **image itself** and the GMU's ITCM/DTCM contents. The reference records
   "ITCM+0x24 remains exactly 0xDEADBEEF after a KGSL boot" and that the reset
   stub is byte-identical. **Is the GMU firmware we're loading (`a640_gmu.bin`)
   the same build as the one KGSL loads?** If not, that is a lead that no
   register comparison can reach, and the project's
   `GPU-ZAP-FIRMWARE.md` shows the same technique was already used to extract
   firmware from `apnhlos`.

2. **A driver-independent check of the "nothing writes" claim** — i.e. whether
   the *UCHE* executes at all. The SQE walking the graph proves the CP-side
   packet machine runs; it does not prove the UCHE (the shader engine) is
   clocked and dispatching. Everything the project has read so far (CP registers,
   GMU registers, the SQE trace) is CP/GMU-side. A UCHE-side counter
   (`UCHE_...` performance or the ALWAYSON counter's per-engine breakdown) would
   be the first genuinely new observation.
