# Two requested checks — hw_init diff (clean) and draw test (inconclusive)

Date: 2026-10-12

## Check 1: `a6xx_hw_init` vs upstream — CLOSED, nothing missing

Compared the full set of `REG_A6XX_*` registers written by `a6xx_hw_init()` in
our tree against the donor v4.19:

```
in DONOR but NOT in OURS:  (none)
in OURS but NOT in DONOR:  34 registers
```

**Every register the donor writes, we write.** The 34 extras are our own
additions: the `*_ADDR_MODE_CNTL` block, GBIF/QSB setup, and the readback
diagnostics. `a6xx_cp_init()` is called in both.

So the port is faithful and this avenue is closed. A missing hw_init step is
**not** the fault.

## Check 2: real draw vs clear — INCONCLUSIVE (and why)

Extended `draw1.py` with the readback it was missing, then ran it on the live
GPU (uptime 1190s, well past the 300s deferral):

```
RENDERER=FD640
FBO=0x8cd5
  shader 0x8b31 compile=1
  shader 0x8b30 compile=1
  link=1
  glDrawArrays err=0x0
  glReadPixels err=0x0 centre pixel = (0, 0, 0, 0)
  VERDICT: DRAW PRODUCED BLACK
```

A real triangle draw — separate code path from the blit, going through the
shader/render pipeline — also produces black.

**But this proves nothing on its own**, because `glReadPixels` is known-broken
(established and validated: a CPU-uploaded known pattern through a COMPLETE
framebuffer reads back as zeros). A correct draw would look identical. The test
cannot discriminate "draw doesn't render" from "readback doesn't report".

I should not have run it as a discriminator without saying this up front.

## The kernel RB probe: started, then abandoned deliberately

I began a self-contained kernel `a6xx_rb_write_probe()` (allocate a BO, emit a
minimal solid-colour 2D blit straight into the ring, `a6xx_idle()`, read the BO
back from the CPU — no Mesa, no GL, no UBWC). It hit four API mismatches in
this tree:

- `OUT_WFI5` is **not defined anywhere** in the kernel tree (only my new code
  referenced it)
- BO allocation is `msm_gem_new_locked()`, not `msm_gem_object_create()`
- `a6xx_idle()` takes `(gpu, ring)`, not `(gpu)`
- `MSM_BO_GEM_CACHE` does not exist

I **reverted it entirely** rather than force it. Writing a kernel probe by
fighting four unknown APIs risks a boot-time crash, and the trigger window is
already freeze-prone.

## Why the kernel probe is now largely redundant

The probe's purpose was to read GPU memory from the CPU, bypassing the broken
readback. **Image 221 already does exactly that**, via the `BOS[]` dump, and
with the raised cap it produced a definitive answer:

- UBWC case: `bo[1] iova=0x25000 ndwords=5120` — metadata plane 0 non-zero,
  **colour plane 0 non-zero**
- `noubwc` case: `bo[1] iova=0x25000 ndwords=4096` — **4096 dwords, 0 non-zero**

The latter is the control that matters: no metadata plane, no compression, so a
successful write cannot hide as RLE-compressed near-zero data.

**The render target is unwritten. That is measured from the CPU and does not
depend on `glReadPixels` at all.**

## Where we actually are

| layer | state |
|---|---|
| bring-up | `BABEFACE ms~1`, `hw_init ret=0`, `FD640` |
| `a6xx_hw_init` faithfulness | **verified complete** vs donor |
| Mesa's nested IB execution | **proven** — `CP_EVENT_WRITE` timestamps increment per submit |
| 2D blit command stream | complete & correct incl. `RB_DBG_ECO_CNTL=0x04100000` |
| colour target address | correct (`0x26000` = `0x25000`+`0x1000`, a 5-page BO) |
| clock blocks that exist | programmed correctly |
| `glReadPixels` | **broken** — validated (CPU-uploaded known data reads as zeros) |
| **render target contents** | **entirely zero, UBWC and non-UBWC, CPU-verified** |

## The remaining question

CP-domain writes land. RB-domain writes do not. Commands are correct, the
address is correct, the buffer is mapped and clocked, and `hw_init` is
faithful.

So: **why does the RB not write?** The leading candidates are now narrow:

1. **The RB is not actually initialised for 2D operation.** `PC_MODE_CNTL` /
   `RB_MODE_CONTROL` state after init has never been verified. We log
   `CP_RB_CNTL = 0x20c` but never compare the PC/RB mode registers against a
   known-good value.
2. **RB error reporting is being read but not interpreted.** We have
   `RBBM_STATUS` and `CP_INTERRUPT_STATUS` (both clean/idle). `RB_CNTL`/`RB_DEBUG`
   and the RB error interrupt masks have not been read after a blit.
3. **The RB writes somewhere other than where we look** — e.g. the address the
   RB dereferences differs from the one we programmed, due to an address-mode or
   `ADDR_MODE_CNTL` mismatch.
4. **Compare against a known-good init.** KGSL drives this same silicon and is
   present in many Android trees; diffing its a640 init sequence against ours is
   the most likely way to find a missing step, since our `hw_init` is faithful
   to *CAF msm* but CAF msm itself was never validated on this board.

(4) is the strongest and has not been tried. It explains "faithful to donor
but still broken" better than any of the others.

## Instrumentation note

The 8192-dword dump cap **wraps the dmesg ring buffer**. Captures must be
narrowed (dump only the render-target BO) or the cap reduced for command
streams, or early submits are lost. This already cost us two runs.