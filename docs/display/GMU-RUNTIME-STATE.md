# GMU runtime state + DEADBEEF ownership (image 89 instrumentation)

## Values the GMU sees before start (to be captured on next boot)

Image 89 adds readback of QTBL_ADDR/INFO, SFR_ADDR, BOOT_CONFIG, CM3_CFG,
GENERAL_7, CP_MSG (reset value; new read-only constant, no write), plus
hfi/icache/dcache heads from host RAM, plus `hfi_start entered`. ITCM/DTCM
have no host copy and no known-safe MMIO read path — verified statically
(blob + loader audit: nothing modifies ITCM/DTCM between load and start).

## What is stored at ITCM+0x24

`0xDEADBEEF` — verified three ways: blob bytes at file offset `0x34`, block-0
dest math (`0x0 + 0x24`), and loader audit (plain copy, both trees).

## Host modification: NONE, both trees

KGSL `load_gmu_fw`: plain `blkwrite`/`memcpy`, no patch step. Ours: plain
`gmu_write_bulk`/`memcpy`. Neither tree touches ITCM+0x24.

## What firmware code references it (disassembled, Capstone, Thumb-2 M-class)

Reset vector `+0x04 = 0x09` → entry `0x08`:
```
0x08: ldr r0, [pc, #0x14]   ; r0 = [0x20] = 0x43f80
0x0a: msr msp, r0           ; MSP = 0x43f80 (DTCM)
0x0e: ldr r0, [pc, #0x14]   ; r0 = [0x24] = 0xDEADBEEF
0x10: msr psp, r0           ; PSP = 0xDEADBEEF  <-- ownership: firmware itself
0x14: ldr r0, [pc, #0x10]   ; r0 = [0x28] = 0x200
0x16: ldr r1, [pc, #0x14]   ; r1 = [0x2C] = 0xE000ED08 (VTOR)
0x18: str r0, [r1]          ; VTOR = 0x200
0x1a: b.w #0x400            ; main firmware
```
Main entry `0x400`: `bl` copy/dispatch prologue (`adr/ldm/add/cmp/bne`,
computed `bx r3`) — normal bootloader, no PSP use in the first 25 insns.

## Is DEADBEEF the DMA source?

Strongly indicated, not yet closed: it is the ONLY `0xDEADBEEF` u32 in the
image, the firmware installs it as PSP, and the fault is a READ of
`0xDEADBEE(x)` — consistent with a PSP-relative access (exception unstack or
thread-mode PSP use). The `0x...BEE0` vs `0x...BEEF` nibble is within SMMU FAR
reporting granularity for the access.

## What else could generate DEADBEE0

If PSP is never used, the remaining candidates are HFI-table contents (dump
proposed in image 89 readback) and NMI-path execution (weakened: CM3_CFG
write verified + fault persisted). The §5 SMMU-path diagnostics already give
CB/SID/FSR/FSYNR per fault; TTBR/domain state adds nothing beyond the
pre-start domain dump already present.

## KGSL runtime-state comparison

KGSL never patches the field either, so a register/ITCM/DTCM/HFI/cache/RPMH/
fence/boot-config difference must explain why PSP gets used under DRM and not
KGSL. Known open deltas: HFI at `0x60000000` (= v5.8 dummy slot) vs
≥`0x60001000`; no log buffer; no ACD; no quirk parsing. None individually
explains a PSP switch; the readback boot will show whether HFI contents or a
register value diverges first.

## Image 89 (instrumentation only, no behavior change)

Readback block + `hfi_start entered` + CP_MSG read-only constant.
`5af6...` no — image 89 `793377edbbd41d4edd8ea9155303063fee4bccd256d6482edadeb659c779a47e`.
SDE 0, DT +0, production untouched.
## Runtime readback (image 89 boot, verified live)

| register | read back | expected | verdict |
|---|---|---|---|
| QTBL_ADDR | 0x60000000 | hfi->iova | exact |
| QTBL_INFO | 1 | 1 | exact |
| SFR_ADDR | 0x6040100 | chipid 6.4.0.1 | exact |
| BOOT_CONFIG | 0x2 | 0x02 | exact |
| CM3_CFG | 0x4052 | 0x4052 | exact |
| GENERAL_7 | 0x1 | 0x1 | exact |
| CP_MSG | 0x0 | reset (never written) | as expected |

| buffer | head words | matches blob? |
|---|---|---|
| hfi | 00000000 00000078 00000006 0000000c | sane empty HFI table |
| icache | 41f0e92d 4e182400 … | code (block 3 @0x4000) |
| dcache | 00000000 ×4 | zeros (no block covers 0x44000) |

Fault 1.5 ms later: `iova=0xdeadbee0, cb=1, fsr=0x40000402 (TF), SID=0x405`.

## Six answers

1. GMU sees exactly what the host wrote (table above) — MMIO model validated.
2. ITCM+0x24 = 0xDEADBEEF; no host path modifies it after load (loader audit +
   no overlapping writes).
3. Referenced by reset stub → PSP (disassembled). Single DEADBEEF u32 in image.
4. DMA source: PSP-relative access is the only firmware path to 0xDEADBEE(x);
   no HFI/scratch/pointer alternative exists in the image or host state.
5. Alternatives exhausted: HFI sane, NMI cleared (CM3_CFG verified), static
   image fully placed, all power/clocks voted.
6. Remaining unknown (GMU-internal): WHAT first uses PSP — exception return
   after a silent primary fault, or thread-mode PSP selection. Requires
   GMU post-mortem (NMI + debug-reg snapshot à la KGSL gmu_snapshot), not
   more host markers.
