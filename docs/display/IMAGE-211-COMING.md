# Image 211 — strip the harness out of the critical path, keep the measurement

Date: 2026-11-11. Built, packaged, **not yet flashed**. Removes our own wedging
diagnostic; keeps the one measurement that matters.

## Why

Image 210 gave the first hard evidence about why runs keep dying. From
`captures/freeze210-last_kmsg.log`:

```
[  64.695312] a6xx-load:1148  CPDUMP[auto-rest] ... ---- end ----
"auto-post" occurrences: 0
```

`auto-rest` completed at t=64.7. `auto-post` was due at ~t=76.7 after
`msleep(12000)`. It never ran, and the `a6xx-load` thread stops emitting at
exactly that point — so it is wedged taking `dev->struct_mutex`, or inside
`a6xx_cp_state_dump()`'s register reads.

**That is our own diagnostic, added in image 186, running on the critical path
twelve seconds after every single bring-up.** Worst possible placement, and the
only thing we have evidence for wedging.

Meanwhile the system kept running to t=491 — eight minutes past bring-up — so
the "hang" is a stuck thread, not a machine death. Image 210 also proved the
re-init theory dead (`handoffs: 1`, `REFUSING: 0`), so the 210 guards stay as
cheap insurance but addressed a hypothesis the evidence eliminated.

## Changes

**1. `auto-rest` / `auto-post` removed** (`load_gpu_thread`, `msm_drv.c`)

Zero `a6xx_cp_state_dump()` calls remain in the load path. They already gave us
`rb_iova=0x1000`, `RB_CNTL=0x20c`, `RB_RPTR_ADDR=0x0` on an earlier run; they
are not earning their keep and every boot costs a bring-up thread.

If live register state is needed again it should be taken **from userspace on
demand** via the `a6xx_gpu_cpstate` node, never scheduled automatically from
inside the load path.

**2. `RINGHW[10]` — three printks condensed to one**

Same information (memptrs_iova, `rbmemptr(rptr)`, `rbmemptr(fence)`, aspace,
mmu, gmu_domain) in one `pr_info`. Renumbered from `04` because `RINGHW[04]` was
already taken by a pre-existing `CP_SQE_CNTL` printk — a collision that would
have made the logs ambiguous.

**3. `RINGCPU` kept, unchanged**

Lives in `a6xx_submit()`, not the bring-up path. Dumps the last ≤16 ring dwords
from the CPU side plus `memptrs->fence`/`rptr` for the first three submits. This
is the measurement the whole investigation has been unable to obtain.

## What this settles

| if `RINGCPU` shows… | then |
|---|---|
| real packet headers — `0x7c0000xx`, `CP_EVENT_WRITE_0_EVENT`, the fence IOVA as a payload | CPU side of the ring is **correct**; fault is GPU-side. Promotes `ROOT-CAUSE-MISSING-SMMU-TABLE-UPDATE.md` to primary. Next image adds upstream's submit prologue plus the `CP_SMMU_TABLE_UPDATE_0..3` field macros that do not yet exist in `adreno_pm4.xml.h`. |
| zeros, garbage, or stale leftovers | the CPU is **not** writing the memory the GPU reads; ring coherency is live after all. |

It also validates or kills `CP_SCRATCH_REG2 == 0` as a signal: if the scratch
write is plainly present in the stream and the register still reads 0, we learn
whether that register is even readable.

## Run procedure (this is the part that has been costing us)

Three separate risks have been firing at once. Separate them:

1. Flash, boot. **Do not touch the GPU yet.**
2. Trigger **once**, in its own short command, no retry.
3. Confirm `BABEFACE`.
4. **Copy dmesg to the host now** — bank the bring-up state before anything can wedge.
5. Run `fbprobe.py` to force a real submit.
6. **Copy dmesg again** and read `RINGCPU`.

Gate: `GL_RENDERER == FD640` before any stage result counts.

## Verification

- Build exit 0, no new warnings.
- `a6xx_cp_state_dump()` calls in the load path: 0.
- `RINGHW[10]` unique (no collision with the pre-existing `RINGHW[04]`).
- `RINGCPU` retained, 3 sites.
- Byte-identical to 210 in header (all 10 fields), cmdline, ramdisk, size.

```
artifacts/flash/211-halium-boot-a6xx-nosleep.img
sha256 a18aa0ea98be011705f55d73483417a91a8647d7920bea3105007d4bc2eb9af8
```

## Not changed

Images 187–210 untouched. The 210 init guards remain. No GPU logic altered.
