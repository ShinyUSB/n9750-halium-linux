# Android/KGSL PSP reference (image 62 + Debian + vendor KGSL)

Vehicle: production ShinyKernel (`KGSL=y`) + Halium ramdisk + Debian rootfs,
`/dev/kgsl-3d0` opened from userspace. Full root + debugfs available.

## Part A — runtime reference

GMU firmware boots with **no SMMU panic** (later idle-transition timeout +
FENCE error interrupt are a separate power-management failure on this
Firmware-less Frankenstein setup: no ZAP image, Debian userspace; open
returns EBUSY afterwards).

### ITCM after KGSL boot (from live 948KB `snapshot/dump`)

```
+0x00: 0x00043f80   (identical to static image)
+0x04: 0x00000009   (identical)
+0x08: 0xf3804805   (identical)
+0x0c: 0x48058808   (identical)
+0x10: 0x8809f380   (identical)
+0x14: 0x49054804   (identical)
+0x18: 0xf0006008   (identical)
+0x1c: 0xbf00b9f1   (identical)
+0x20: 0x00043f80   (identical)
+0x24: 0xdeadbeef   (identical — NOT patched)
+0x28: 0x00000200   (identical)
+0x2c: 0xe000ed08   (identical)
```

Reset stub byte-identical to the flashed image. KGSL never touches it.

### KGSL GMU memory map (snapshot section headers)

| object | gmuaddr | size |
|---|---|---|
| HFI | **0x60100000** | 20K |
| dummy/BIN | 0x60000000 | 4K |
| icache | 0x4000 | 240K |
| dcache | 0x44000 | 0x4000 |
| LOG | 0x60200000 | 4K |
| ITCM | 0x0 | 16K |
| DTCM | 0x40000 | 16K |

### HFI table head (KGSL, at 0x60100000)

`version=0 size=0x36 qhdr0=6 qhdr_size=0xc ...` — 4 queues (CMD/MSG/DBG/DSP).
DTCM base matches our tree's contents all but one word.

### Answer to the key question

**YES — ITCM+0x24 remains exactly `0xDEADBEEF` after a KGSL boot.**
DEADBEEF is an intentional firmware sentinel (PSP poison installed by the
reset stub itself). Do not patch the literal. The fault is PSP *use*, and
the question is what triggers it under DRM but not KGSL.

## Part B — KGSL source audit (PSP/PIL/secure/start)

- `PSP`: zero hits in KGSL gpu/msm (excluding sptprac/dsp). No host PSP handling.
- PIL/subsystem in GMU path: none (`a640_zap` is loaded once via
  `subsys-pil-tz` at probe; already known, unrelated to PSP).
- `a6xx_gmu_start` (KGSL): **`WFI_CONFIG = 0` first**, then SYSRESET assert
  (0, no 1→0 pulse), `wmb()`, poll `INIT_RESULT == 0xBABEFACE`.
- Ours: SYSRESET 1→0, poll, **no WFI_CONFIG write** (constant absent from our
  XML; KGSL `A6XX_GMU_CX_GMU_WFI_CONFIG = 0x1F802`).

## Part C — concrete KGSL-vs-DRM divergences (PSP-relevant first)

| # | KGSL | DRM (ours) | PSP relevance |
|---|---|---|---|
| 1 | `WFI_CONFIG = 0` before SYSRESET release | never written | **direct**: controls GMU WFI/sleep-interrupt behavior in the fault window |
| 2 | HFI at `0x60100000`, 4 queues, size in **dwords** | HFI at `0x60000000` (dummy slot), 2 queues, size likely **bytes** (`0x78` = 120 = hdr + 2×48, vs KGSL `0x36` dwords = 216 = hdr + 4×48) | firmware walks queues early; unit mismatch misdescribes the table |
| 3 | dummy page at `0x60000000`, LOG at `0x60200000` | neither | layout expectations |
| 4 | ITCM+0x24 untouched | untouched | none — identical |

## Standing evidence files

- `kgsl-ref/` (empty — sysfs `dump` reads race snapshot release; all analysis
  was done on-device via python3 and transcribed above)
- This file is the record; raw excerpts are inline above.

No Debian kernel changes, no firmware changes, no flashes (image 62 was
already the boot image from the previous step). Production tree untouched.
Next: single-variable test of divergence #1 (WFI_CONFIG) and/or #2 (HFI
units/count), one at a time.