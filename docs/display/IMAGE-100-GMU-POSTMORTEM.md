# Image 100 — GMU post-mortem: firmware runs fully, reports failure, no fault

## BADDFACE first seen @37 ms; system stayed alive (gate holds, no hw_init)

Raw dumps saved: `IMAGE-100-DUMP.log` (PRE 3328 + POST 3328 rows).

## PRE vs POST word diff (1024 ITCM + 1024 DTCM + 256 DEADB + 1024 HFI rows)

- **ITCM: 18 rows differ.** Header `+0x20..0x2C` (incl. DEADBEEF) untouched.
  `+0x00..0x1C` (stub) and `+0x240..0x330` (`0x53d`→`0x2ca9` fills) rewritten
  by firmware self-relocation/copy loops (disassembled `bl #0x408` prologue).
- **DTCM: ~470 rows differ.** Status (`3fff3fff/0fdf0000`), internal pointers
  (`0x42094…`, `0x43384`), full memtest fills (`0xDD…`, `0x44…–0xBB…`),
  descriptor table at `0x40E40+` (stride-16 `{addr,index}` pairs), stack garbage
  at `0x43F80` area. Firmware performed complete boot init.
- **DEADB: 0 differ** (zeros pre and post; zeroed between probe fill and PRE).
- **HFI: 0 differ** (firmware never touched it — no handshake attempted).

## Seven answers

1. **ITCM changes:** stub overlaid + `0x240..0x330` refilled (`0x53d`→`0x2ca9`);
   header/`+0x20` row intact. Normal self-relocation.
2. **DTCM changes:** above; no pointer to `0xDEADxxxx`/`0x600xxxxx` found in
   changed words (pointers are DTCM-internal `0x42xxx`/`0x43384`).
3. **DEADB scratch:** zeros before release AND after; zeroing window remains
   probe→clock-enable (MMIO/IRQ/pwrlevels/hfi/fw-reads/CX/clk — GMU clocked
   but unasserted for part of it). No new narrowing possible from this data.
4. **No DTCM pointer to 0xDEAD/0x600xxxxx** in the diff set.
5. **No pre-BADDFACE structure change isolated:** BADDFACE appears at +37 ms;
   all observed writes (memtest, tables, status) precede it as normal init.
   The verdict follows complete init, not a crash mid-step.
6. **No stack/frame signature of the BADDFACE path** beyond normal boot
   activity; DTCM stack area shows ordinary garbage, no distinctive frame.
7. **BADDFACE correlates with nothing modified except normal boot state.**
   Firmware ran to completion of its init sequence and *decided* failure.

## Prime hypothesis (untested, deferred since image 93)

HFI table validation: ours advertises 2 queues/size `0x1E`; KGSL advertises
4/size `0x36`. A firmware gate on table shape would produce exactly this
signature — full init, deliberate failure verdict, no handshake, no fault.
Discriminating experiment: 4-queue table. NOT implemented here.

Image 100 `745590fa48c5e7226ba661177aafbdb74c565f1ac72b65a5d0d9952de99cb4088`.
SDE 0, DT +0, production untouched. Diagnostic instrumentation only; gate
holds; system stable.