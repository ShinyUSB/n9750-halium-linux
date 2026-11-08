# IMAGE 219 — the nested IBs are RELOCS, not cmd entries

Date: 2026-10-11
Image: `artifacts/flash/219-halium-boot-a6xx-ibtgt.img`
Captures: `artifacts/gpu-bringup/captures/219-run.txt`, `219-preboot.txt`

## Result: IBTGT captured NOTHING, and that is the useful result

Image 219 added a dump for `MSM_SUBMIT_CMD_IB_TARGET_BUF`, on the theory that
image 218's three nested `CP_INDIRECT_BUFFER` targets were submitted that way.

They are not. The capture contains **zero `IBTGT` lines**, and every submit
still shows exactly one cmd:

```
IBDUMP[seq=1] cmd[0] type=1 size=346 iova=0x2a000
IBDUMP[seq=2] cmd[0] type=1 size=332 iova=0x2b000
IBDUMP[seq=3] cmd[0] type=1 size=1 size=223 iova=0x2a000
```

All `type=1` (`MSM_SUBMIT_CMD_BUF`). **`nr_cmds == 1`** — there are no
`IB_TARGET_BUF` cmd entries in these submits at all.

Results are otherwise identical to 218, and stable across a different boot:

| submit | dwords | 218 | 219 |
|---|---|---|---|
| seq=1 | 346 | 346 | 346 |
| seq=2 | 332 | 332 | 332 |
| seq=3 | 223 | 223 | 223 |

Same sizes, same boot-to-boot. So the 218 capture was representative; the only
thing wrong with it was *which buffer we were looking at*.

## The actual reason, and the blind spot it reveals

A nested indirect buffer is not a cmd entry — it is a **reloc**.
`fd_ringbuffer_emit_reloc_ring_full()` records the target buffer in
`submit->bos[]` and patches its address into the `CP_INDIRECT_BUFFER` packet.
There is no `cmd[]` record for it, so iterating `nr_cmds` can never find it,
which is exactly what image 219 demonstrated empirically.

`struct msm_gem_submit` (msm_gem.h:196-209):

```c
	unsigned int nr_cmds;
	unsigned int nr_bos;
	struct {
		uint32_t type;
		uint32_t size;      /* in dwords */
		uint64_t iova;
		uint32_t idx;       /* cmdstream buffer idx in bos[] */
	} *cmd;                /* array of size nr_cmds */
	struct {
		uint32_t flags;
		struct msm_gem_object *obj;
		uint64_t iova;
	} bos[0];              /* <-- the real render commands live HERE */
```

So the three-step chase has been:

| image | dumped | result |
|---|---|---|
| 217 | the ring | envelope only; gated on `seqno <= 3` so it was also stale |
| 218 | `cmd[]` | one buffer per submit; itself full of nested IB refs |
| 219 | `cmd[]` IB targets | nothing — there are no such entries |

Every step has missed by one indirection. The commands have been in `bos[]`
the whole time.

## Image 220

Walks `submit->bos[]` directly, printing `{iova, flags, obj}` for every entry,
and dumps contents only for objects <= 1024 dwords. That bound matters: the
64x64 RGBA8 render target is 4096 dwords and would otherwise swamp the log,
while the nested IB targets are only 18-93 dwords.

Expected, if the chase finally lands: `RB_MRT_BUF_INFO(0)` (0x8822) carrying the
colour target, and `RB_BLIT_INFO` (0x88e3) with `CLEAR_MASK` plus
`RB_BLIT_CLEAR_COLOR_DW0..3` (0x88df) for the GMEM fast-clear — none of which
has appeared in any decode so far.

## Status

Still `first pixel RGBA = (0,0,0,0)`, `GL_RENDERER = FD640`. Phone did NOT
freeze on this boot (boot_id `c14d1a67`, GPU trigger and GL run completed
cleanly), so the freeze is intermittent rather than deterministic.

## Note on method

Three consecutive images missed by one level of indirection, each because I
trusted the previous image's framing of where the data "should" be. The
general lesson: when a dump shows something self-contradictory, the cheapest
hypothesis to test first is that the dump is pointed at the wrong object —
not that the driver is misbehaving. Two of the three "findings" (empty
framebuffer, missing MRT) were artefacts of that.