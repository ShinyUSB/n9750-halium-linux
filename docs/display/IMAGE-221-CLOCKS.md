# IMAGE 221 — clock check done, and the render target is DEFINITIVELY unwritten

Date: 2026-10-12
Image: `artifacts/flash/221-halium-boot-a6xx-clkchk.img`
Boot: `26271774-2dc2-40df-bb3d-0b2bfd02abb6`
Captures: `artifacts/gpu-bringup/captures/221-run.txt`, `221-preboot2.txt`

## 0. OPERATIONAL FIX: do not trigger the GPU before the deferral expires

The phone froze twice on this image. The second freeze happened with
`ibdump=N`, so my printk-flood explanation was wrong.

The real cause: **`a6xx.gpu_init_delay_ms=300000`** (5 minutes).

| uptime at `a6xx_gpu_load` | result |
|---|---|
| ~70–90 s | **FREEZE** |
| ~360 s (past the 300 s deferral) | **`BABEFACE ms~1`, `hw_init ret=0`** |

Triggering inside the deferral window races the deferred init and wedges the
device. **Always wait until uptime > 300 s before writing `a6xx_gpu_load`.**
Note that past 300 s the deferred init still has *not* fired on its own (no
`BABEFACE`, cpstate `nogpu`) — the explicit trigger is still required, it just
must not be early.

## 1. Clock/power state — hypothesis weakened

```
CLKCHK[seq=3] CLOCK_CNTL       0x8aa8aa02     <- exactly the wanted value
CLKCHK[seq=3] CLOCK_CNTL2_RB0  0x00002222     <- programmed (hwcg)
CLKCHK[seq=3] CLOCK_CNTL2_SP0  0x02022220     <- matches a6xx_hwcg exactly
CLKCHK[seq=3] CLOCK_CNTL2_TP0  0x22222222
CLKCHK[seq=3] CLOCK_CNTL2_RAC  0x00005555
CLKCHK[seq=3] CLOCK_CNTL_SP0   0x22222222
CLKCHK[seq=3] CLOCK_CNTL2_RB1  0xdeafbead
CLKCHK[seq=3] CLOCK_CNTL2_RB2  0xdeafbead
CLKCHK[seq=3] CLOCK_CNTL2_RB3  0xdeafbead
CLKCHK[seq=3] CLOCK_CNTL2_SP1  0xdeafbead
CLKCHK[seq=3] CLOCK_CNTL2_SP2  0xdeafbead
CLKCHK[seq=3] CLOCK_CNTL2_SP3  0xdeafbead
CLKCHK[seq=3] CLOCK_CNTL2_UCHE 0xdeafbead
CLKCHK[seq=3] GMU_SPTPRAC_CLKCTL 0x00000000
CLKCHK[seq=3] GMU_SPTPRAC_PWRCTL 0x00000000
CLKCHK[seq=3] RBBM_STATUS      0x00000001     <- CP idle
```

`SP0 = 0x02022220` is bit-identical to the `a6xx_hwcg` programming, and
`CLOCK_CNTL = 0x8aa8aa02` is exactly the wanted aggregate. **The clock blocks
that exist on this chip are programmed correctly.**

The `0xdeadbead` reads are a sentinel for unimplemented/reserved registers —
a640 has one RB, one SP, one TP. So those rows are *not* evidence of a disabled
block; they are not real registers. The two GMU `0x00000000` reads are
ambiguous (could be genuine zero, could be another sentinel), but since the
RB/SP/TP clock rows are correct, **"the RB is not clocked" is now unlikely**
and should be demoted.

## 2. The render target is DEFINITIVELY unwritten (correct region this time)

Image 220's conclusion was retracted because the dump cap of `0x1000` bytes
covered only the UBWC metadata plane. Image 221 raised the cap to 8192 dwords.

**UBWC case** — the 5120-dword (`0x5000`) render target at `iova=0x25000`:

```
metadata plane [0:1024]    nonzero = 0
COLOUR   plane [1024:5120] nonzero = 0
```

**`noubwc` case** — no metadata plane, so this is the raw colour plane,
`bo[1] iova=0x25000 ndwords=4096`:

```
dwords: 4096   nonzero: 0
first nonzero: []
```

This is the control that matters. With UBWC disabled there is **no metadata
plane and no compression**, so a successful write cannot hide as RLE-compressed
near-zero data. The colour plane is **completely untouched**.

## 3. Where we now stand

Everything upstream of the write is verified correct:

| check | result |
|---|---|
| bring-up | `BABEFACE ms~1`, `hw_init ret=0`, `GL_RENDERER=FD640` |
| Mesa's nested IB executes | **proven** — `CP_EVENT_WRITE` timestamps increment once per submit |
| 2D blit command stream | complete & correct, incl. `RB_DBG_ECO_CNTL=0x04100000` (`BLIT_OP_SCALE` workaround) around `CP_BLIT` |
| colour target address | correct — `0x26000` = `0x25000` + `0x1000` UBWC offset of a 5-page BO |
| clock blocks that exist | programmed correctly |
| **render target contents** | **completely zero, UBWC and non-UBWC** |

So: CP-domain writes land (control memory increments), every register in the
blit is right, the address is right, the buffer is mapped and clocked — and the
target is **never written**.

That is a narrow, well-characterised failure: **the RB/SP render pipeline does
not produce writes, even though the CP runs and the commands are correct.**

## 4. What is left to investigate

1. **`a6xx_hw_init` vs upstream.** Diff our register writes against mainline
   `a6xx_hw_init()`. A missing RB/PC/SP init step would look exactly like this:
   commands accepted, nothing drawn. Worth doing next — it is local and needs
   only a diagnostic image.
2. **`CP_ME_INIT` / `PC_MODE_CNTL` state after init.** Verify the render block
   was actually put into a mode that accepts 2D blits.
3. **Probe the RB directly**: write a known value via `RB_2D_DST_INFO` to a
   scratch buffer and read it back — no Mesa, no shaders, minimal command
   stream. That would finally isolate "RB writes nothing" from anything in Mesa.

## 5. Instrumentation note

At the 8192-dword cap the printk volume **wraps the dmesg ring buffer** — the
earlier submits were gone before we could read them. The next image should dump
only the render-target BO, or drop the cap for command streams, so captures
survive.