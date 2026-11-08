# Image 99 — GMU firmware ABI audit (read-only; no source/build/flash changes)

## PART 1 — DTCM+0xFF8 version check (recovered from current upstream)

Current mainline `a6xx_gmu_start()` reads `CM3_DTCM_START + 0xff8` BEFORE
reset: value ≤ `0x20010004` → old ABI, poll `INIT_RESULT == 0xbabeface`
(full mask); newer → poll `(INIT_RESULT & 0x1ff) == 0x100`.

Our Samsung blob: DTCM+0xFF8 falls in block 4 (`0x40000+0x1b20`), value
**0x00000000** → old ABI. Our poll (`== 0xbabeface`) is CORRECT for this
firmware. Missing logic documented; it would not change our outcome.

## PART 2 — verdict-value locations

| value | file offset | block → runtime addr | position |
|---|---|---|---|
| `0xBADDFACE` | 0x1c78 (sole occurrence) | block 2 → **ITCM 0x1ED8** | literal pool after padded function end |
| `0xBABEFACE` | 0x6cec (sole occurrence) | block 3 → **icache 0x7280** | same pool pattern |

No PC-relative load in ITCM targets 0x1ED8; no `movw/movt` in any block
constructs `0xBADD/0xBABE/0xFACE`. Both values are pool-positioned data, not
inline code.

## PART 3 — failure condition: NOT statically resolvable, reason identified

Callers of the function owning the BADDFACE pool: none found — startup
dispatch goes through a computed `bx r3` (disassembled at 0x432), i.e.
table-driven calls invisible to static branch tracing. Peripheral addresses
are likewise table-built (no recognizable immediates for the INITRES
store). The decision branch cannot be located with available tooling.

## PART 4 — success/failure values and ABI

- Failure: `0xBADDFACE`; success: `0xBABEFACE`. Both single pool entries.
- ABI: OLD (PART 1) — our `== 0xbabeface` poll matches; no condition change.
- `BOOT_CONFIG`: 0x02 everywhere (ours, v5.8, KGSL `TCM_BOOT` = 2). Match.

## PART 5 — expectation table (firmware vs DRM vs KGSL)

| expectation | evidence | DRM | KGSL | verdict |
|---|---|---|---|---|
| DTCM+0xFF8 version gate | upstream source | absent (would pick old ABI anyway) | N/A (own loader) | no effect |
| `== 0xbabeface` poll | upstream + blob (=0 → old) | match | N/A | correct |
| BOOT_CONFIG = 2 | all three sources | match | match | correct |
| ITCM+0x24 poison | blob + disasm (PSP) | unpatched | unpatched | identical |
| CP_MSG/log | upstream + KGSL | absent | present | deferred (faults not log-adjacent) |
| HFI/dummy layout | upstream map | differs | differs from upstream too | ruled out (img 91) |

## Conclusion

The firmware side is fully mapped and every host-programmed value matches.
The BADDFACE/BABEFACE decision lives behind table-driven dispatch that
static analysis cannot penetrate with available tools. Nothing further can
be extracted without runtime GMU introspection (unavailable) or the HFI
handshake completing (blocked on the boot verdict itself).

No kernel/DT/firmware changes made. Nothing built or flashed. Production
untouched. SDE 0 by zero modification.