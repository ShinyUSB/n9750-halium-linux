# Adjudication: did the `CP_REG_TO_MEM` macro bug cause the image-266 hard reset?

Date: 2026-10-16. Adjudicated by the main session after `beam-PLAN.md` and
`PLAN.md` (mimo) reached **opposite conclusions**. Verdict below is checked
against primary sources, not memory.

## The disagreement

- **beam** (`beam-PLAN.md` §5): the image-266 reset was an *encoding* bug —
  `adreno_pm4.xml.h` puts `CNT` at bit 19 while the a6xx XML says bit 18, so
  `CNT(2)` is read by hardware as `CNT(4)`, over-reading the packet.
- **mimo** (`PLAN.md` §2): upstream emits the identical packet and it is a
  production path, so images 264–266 failed for reasons *other* than encoding.

## What is verified

| Claim | Status | Evidence |
|---|---|---|
| CAF tree header: `CNT` mask `0x3ff80000`, shift **19** | **confirmed** | `ShinyKernel-gpu-exp/.../adreno_pm4.xml.h:722-723` |
| A6xx XML: `REG` bits 0–17, `CNT` bits **18**–29, `64B` bit 30 | **confirmed** | `/tmp/opencode/regs/mesa-pm4.xml:1080-1086` |
| image 266 emits `REG(r) \| CNT(2) \| 64B` | **confirmed** | `a6xx_gpu.c:745-747` |
| upstream master emits the **identical** packet | **confirmed** | `torvalds/linux` `a6xx_gpu.c:222-229` `get_stats_counter()` |
| upstream calls it **unconditionally in `a6xx_submit()`** | **confirmed** | upstream `a6xx_gpu.c:372,380` — every submit, not debugfs-gated |
| Mesa emits the same macro family for counters | **confirmed** | `fd6_query.c:425-427`, `tu_cmd_buffer.c:2449` |
| upstream's `adreno_pm4.xml.h` bit layout | **NOT FOUND** | file is no longer committed upstream (build-generated); 404 on raw.githubusercontent and cgit, absent from the GitHub contents API |

## Verdict

**beam's causal claim is not supported. mimo wins this point.**

The packet beam calls malformed is the exact packet upstream `msm` emits on
**every submit on every a6xx device**. If `CNT` at bit 19 made hardware read
4 dwords instead of 2, upstream's per-submit stats path would over-read the
ring and over-write `rbmemptr_stats()` by 4 bytes every submit, on every
device in the field. That is not a subtle bug — it would have been found and
fixed long ago. Therefore hardware accepts this encoding, and the
"unsupported packet / bad encoding" reading of the 266 reset is wrong.

The bit-layout discrepancy (19 vs 18) is **real but latent** — worth a note,
not worth a rebuild, and not the reset's cause.

## Better hypothesis for the 266 hard reset (untested)

Encoding is not the problem, so the reset needs another cause. The strongest
remaining candidate is **destination aliasing**, not packet format: the
capture wrote into the `memptrs` tail to avoid allocating a BO (deliberately,
to escape the 264/265 `struct_mutex` deadlock). `memptrs` is live driver
state — it carries `clear`, `retire`, `mem_retired`, and the ring
`wptr`/`rptr` mirror. Writing the capture at a tail offset can clobber fields
the CPU and the CP both depend on, which is a far better explanation of a hard
reset than an unsupported opcode.

Note this compounds cleanly with the bit-layout oddity: if hardware really does
read `CNT` from bit 18 (as the XML says), a `CNT` value encoded at bit 19 is
doubled (2 -> 4), so the capture would write **8 bytes wider than intended** —
landing squarely on live `memptrs` neighbours. Both readings point at the same
mitigation: **capture into a dedicated BO, and never into `memptrs`.**

## Actionable consequence

- Do **not** rebuild the 266 instrument on the assumption that fixing the shift
  fixes the reset. That fixes nothing.
- If the register-capture idea is revived, the design requirement is a
  *dedicated destination buffer*, and it must be allocated where it cannot
  deadlock (`msm_gpu_init()` before `hw_init`, the way upstream orders
  `a6xx_ucode_load()`), or preallocated at module load.
- The cheaper alternative remains preferable: `RBBM_STATUS` (`0x210`,
  `REG_A6XX_RBBM_STATUS`, a **global** register, so not context-banked) added
  to the existing change-driven tracer. One MMIO read, no packets at all, and it
  cannot wedge the device.

## Files

- `consult/beam-PLAN.md` — plan (its Experiment B rationale needs this correction)
- `consult/PLAN.md` — mimo plan (correct on this point; its X1 aperture lead stands)
- `consult/musespark-SILICON.md` — WFI-on-idle-RB mechanism, RBBM_STATUS test