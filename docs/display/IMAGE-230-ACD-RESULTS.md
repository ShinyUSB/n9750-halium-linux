# 🎉 IMAGE 230 — THE POWER WALL IS BROKEN. ACD notification to AOP was the missing link.

Date: 2026-10-12
Image: `artifacts/flash/230-halium-boot-a6xx-acd.img`
sha256: `25e7de4df45bc3f79695a8597c94fe0c9e7f477aa5df7363a4ec824eeaac86e9`
Boot: `6e2b1d4b`, kernel `#37`, device alive
Cmdline: `a6xx.force_gx_clk=0 a6xx.send_acd=1` — **only the ACD change under test**

## The result

```
ACD230: AOP mailbox channel acquired
ACD230: '{class: gpu, res: acd, value: 1}' len=32 mbox_send_message ret=0
gpu_cc_gx_gmu_clk   en=1  prep=1  rate=200000000
```

**`gpu_cc_gx_gmu_clk` is enabled.** In 228 previous images it had never once
been `en=1`. `force_gx_clk=0`, so this is the ACD notification doing it — not the
brute-force register write.

The port is transcribed from vendor KGSL `drivers/gpu/msm/kgsl_gmu.c:1255`
(`/home/usb/ShinyKernel/Kernel`, the Samsung SM8150/d2q 4.14 tree for this exact
board), which was found on disk after 228 images of not finding it:

```c
msg.len = scnprintf(msg_buf, sizeof(msg_buf),
		"{class: gpu, res: acd, value: %d}", state);
msg.msg = msg_buf;
ret = mbox_send_message(mailbox->channel, &msg);
```

placed at the **top** of `a6xx_gmu_resume()`, before `pm_runtime_get_sync()` —
matching KGSL, which does it before GDSC, before clocks, before the RPMh vote.

Two details that were decisive and would have silently failed if guessed wrong:

- the key is **`value`**, not upstream msm's `val`
- KGSL sends **1 (ACD ON)**, because `gmu_aop_mailbox_init()` does
  `set_bit(ADRENO_ACD_CTRL)` and the cached state starts false. My earlier
  assumption that this was an "off notification, zero risk" was backwards.

## Second first: the GPU now raises interrupts

```
[393.776864] adreno ...: [drm:a6xx_fault_detect_irq] *ERROR* gpu fault ring 0
            fence 6 status 00E51005 rb 0144/0144 ib1 000000000002B000/0000
```

For the entire project the GPU IRQ count was **0** and `irq_handler` never ran
(`ROOT-CAUSE-GPU-IRQ-NEVER-FIRES.md`). It now delivers, the hangcheck fires, and
a real fault status is reported. That alone would have been worth an image.

Decoded against `a6xx.xml.h:506-529`:

| bit | mask | meaning |
|---|---|---|
| 0 | `0x1` | `CP_AHB_BUSY_CX_MASTER` |
| 2 | `0x4` | **`CP_BUSY`** |
| 16 | `0x10000` | `VFD_BUSY` |
| 18 | `0x40000` | `SP_BUSY` |
| 20 | `0x100000` | `VPC_BUSY` |
| 21 | `0x200000` | `GPU_BUSY_IGN_AHB_CP` |
| 22 | `0x400000` | `VSC_BUSY` |
| 23 | `0x800000` | **`HLSQ_BUSY`** |

`RB_BUSY` (0x80) is **clear**. `RB_RPTR == RB_WPTR` (0144/0144).

## The problem is now a completely different one

Before: the clock was off, nothing ran, no interrupts, silence.
Now: the clock is on, the GPU is **busy executing**, and the command processor
never retires.

So this is no longer a power problem. It is a **command execution hang**:
`HLSQ_BUSY` + `CP_BUSY` with the ring fully drained means the CP walked into
work it cannot finish. `pixelreadback.py` sits in `dma_fence_default_wait`
forever, and the kernel hangcheck correctly reports it.

That is a genuinely tractable, ordinary problem — and it is a completely
different class from 228 images of "the GPU is dark".

## What the ACD result retires

| retired claim | evidence |
|---|---|
| "the GX rail is never voted" | GDSC transitioned on 225, and the clock now opens |
| "host must `regulator_enable(gpu_gx_gdsc)`" | 226: ret=0, gate stayed shut — irrelevant |
| "force the gate bit" | unnecessary; ACD opens it properly |
| "bit 28 is an unexplained GMU→host event" | 226: both H2G_RAW and G2H_INFO now zero |
| "OOB never acks" | acks on every resume since 225 |
| `GPU_CC+0x162/0x0F` is the gate | **wrong** — it is a measurement register; the real gate is `0x1064 BIT(0)` |

The single missing instruction was a **32-byte ASCII string to the
Always-On Processor**, sent before any power or clock resource is touched.

## Next steps

The fault is now `CP_BUSY` + `HLSQ_BUSY` with an empty ring. Next readings:

1. `RBBM_STATUS3`, `CP_INTERRUPT_STATUS`, `CP_RB_CNTL`, `PC_STATUS` at the
   moment of the hang — what is the CP actually executing?
2. Whether the hang is at a specific packet (compare `RB_WPTR` at hang against
   the submission) or on every submission.
3. `CHECKS-HWINIT-DRAW.md` candidate (4), now much more likely to be the real
   answer: diff our `a6xx_hw_init` against KGSL's a640 init — and KGSL source
   **is now available on disk**, which it was not when that was written.
4. The GMU SQE firmware (`a630_sqe.fw`) is separately loaded; the CX-side CP
   running while GX-side stalls is worth separating explicitly.

## Process notes worth keeping

- **The vendor source was on disk the whole time**, in a second kernel tree at
  `/home/usb/ShinyKernel/`. A subagent found it. Nobody had looked there; the
  project's `kgsl-ref/` was empty and `ANDROID-KGSL-PSP-REFERENCE.md` recorded
  "kgsl-ref empty — do not waste time there". That conclusion was about the
  wrong directory.
- **A bootloop cost an image and nearly cost the device.** Image 230's first
  packaging shifted the ramdisk 40 bytes out of page alignment
  (`48422912` → `48422952`); the bootloader could not find it. No GPU log
  existed because the kernel never ran. Structural comparison against a
  known-good image found it immediately. Never hand-shift a boot image.