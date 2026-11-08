# GMU DEADBEEF address-handoff audit (read-only, no source changes, no flash)

## 1. Host→GMU address handoff table (fw-load → CM3_SYSRESET)

| register / struct | value written | meaning | firmware-dereferenceable? |
|---|---|---|---|
| GENERAL_7 = 1 | TCM retention | no | no |
| PDC SEQ/TCS (rpmh_init) | sequence blobs | PDC/RPMh programming | no (PDC, not GMU-DMA) |
| ITCM/DTCM blocks | image bytes via MMIO apertures | firmware code/data | indirectly (it IS the code) |
| icache/dcache blocks | memcpy to bos | firmware code/data | indirectly |
| INIT_RESULT = 0 | handshake flag | status | no |
| BOOT_CONFIG = 2 | TCM boot mode | boot mode | no |
| QTBL_ADDR = 0x60000000 | HFI table IOVA | **YES — firmware's first DMA target** | mapped ✓ |
| QTBL_INFO = 1 | table info | config | no |
| FENCE_RANGE_0 | AHB window | bus fence | no |
| CM3_CFG = 0x4052 | NMI clear | boot correctness | no |
| SFR_ADDR = chipid | chip ID | info | no |
| CP_MSG | **never written** | log addr/size | **unknown (reset value)** |
| power_config regs | votes, sptprac | power | no |
| CM3_SYSRESET 1→0 (in start) | release reset | boot | no |
| GX/MX_VOTE_IDX (rail_on) | ARC votes | power | no |

Host-side IOVA structures: hfi (0x60000000/16K), icache, dcache. No log, no
dummy, no scratch.

## 2. DEADBEEF in source: NONE

Zero occurrences (either endianness, any case) in: experimental DRM/MSM,
production DRM/MSM, KGSL. The poison is firmware-blob-originated only.

## 3. DEADBEEF in `a640_gmu.bin`: exactly ONE u32

File offset `0x34` (`EF BE AD DE` bytes = u32 `0xDEADBEEF`). It sits in block 0
(ITCM dest `0x0`, size `0x30`): header `[0x0,0x10)` + data `[0x10,0x40)`, so the
value lands at **ITCM+0x24** after load.

Block 0 looks like a header, not code: `+0x0: 0x43f80`, `+0x20: 0x43f80`,
`+0x24: DEADBEEF` (`0x43f80` itself points into DTCM runtime/scratch with no
backing image data — normal BSS-style use).

Block-content pointer scan: no other GMU-region pointer plausibly read as an
address (remaining hits are Thumb code immediates). DTCM block 4 contains
`0x60000400` (HFI-region pointer — inside our mapped HFI buffer).

## 4. KGSL vs DRM around start

KGSL `load_gmu_fw` does plain `blkwrite`/`memcpy` — **it does NOT patch
ITCM+0x24 either**. So either the field is dead under KGSL too (and our fault
has another source), or KGSL's environment avoids the dereference. No evidence
currently distinguishes these.

KGSL/fw_start register set vs ours differs now ONLY by: CP_MSG (+log buffer),
dummy page, ACD, bus vote, quirk handling — all previously catalogued.

## 5. Proposed readback instrumentation (NOT applied)

Before start: read back QTBL_ADDR, SFR_ADDR, CP_MSG, BOOT_CONFIG, GENERAL_7
(all safe MMIO reads of registers we wrote). After the fault (via last_kmsg
on next boot): dump our HFI buffer content (host RAM at 0x60000000 mapping)
to see what the firmware wrote/read. This connects host state → GMU DMA
without guessing.

## 6. Block destinations (confirmed, not redone)

All 7 blocks land in ITCM/DTCM/icache/dcache; zero unmatched-block prints.
Fault IOVAs (`0x433c0/0xb54e0/0x81ce0/0x89ce0/0xdeadbee0`) match no block
destination and no mapping.

## 7. Samsung vs upstream ABI

Same filename ≠ same ABI remains formally open, but the concrete evidence
narrows it: KGSL programs NO per-field patches into the image, so a
Samsung-specific host-patch step would have to live in registers — and the
register sets now match except CP_MSG/log, dummy, ACD, bus vote. Of those,
only CP_MSG carries an address the firmware could dereference, and its reset
value would fault near `0x0`, not at these addresses. The DEADBEEF field
(ITCM+0x24) is the single unexplained firmware-visible pointer, and no tree
patches it.

## Verdict on the central question

*"What firmware-visible pointer should the host init, and why is it
DEADBEEF?"* — Candidate identified (ITCM+0x24 header field), mechanism NOT
proven: KGSL doesn't patch it either, so host-patching cannot be declared the
fix. Do NOT write a guessed value there. Next: the §5 readback/dump to see
whether the GMU ever touches ITCM+0x24's content vs. reading HFI/scratch.

No source/DT/IOMMU/power/firmware/HFI/fence changes made. Nothing built or
flashed. Production untouched.