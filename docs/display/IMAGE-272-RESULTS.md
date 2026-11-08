# Image 272 results — and a retraction

Date: 2026-10-18
Evidence: `artifacts/gpu-bringup/evidence-272/phone-20261020-140828.txt` (2.3 MB:
the harness output, the kernel's debugfs BO table, and a *continuous* dmesg
capture, all from one run)

## 1. RETRACTION: the shader ISA **is** submitted, and the programs are delivered

Earlier today I concluded — from the image-271/272 pointer map — that the
shader ISA buffers were "neither submitted nor referenced". **That was wrong,
and the reason was an incomplete capture.**

The `ibdump` content dump is ~11 000 printk lines per submit, which **evicts
the dmesg ring buffer**. The submits that carry the draw were the *earliest*
ones, and they were the first to be evicted. What survived was the later
clear/blit submits, which contain no `CP_DRAW` and no `CP_LOAD_STATE6` at all.

Re-run with `dmesg -W` streaming to a file (nothing evicted), the picture is
the opposite:

```
seq=1  (12 BOs captured in full)

  CP_LOAD_STATE6_GEOM  @dword 470   payload: 0x00720000 | 0x00030000 0x00000007
                                     -> address 0x700030000  = VS2 ISA
  CP_LOAD_STATE6_GEOM  @dword 1494  payload: 0x00620000 | 0x0002e000 0x00000007
                                     -> address 0x70002e000  = VS ISA
  CP_LOAD_STATE6_FRAG  @dword 1573  payload: 0x00720000 | 0x0002f000 0x00000007
                                     -> address 0x70002f000  = FS ISA
  CP_DRAW              @bo[9] dword 69
```

and the three ISA buffers' own IOVAs, from the kernel's own BO table, in the
same run:

| buffer | mmoff | own IOVA | referenced by the stream |
|---|---|---|---|
| VS ISA  | 0x10217c000 | 0x70002e000 | 2× |
| FS ISA  | 0x10217d000 | 0x70002f000 | 2× |
| VS2 ISA | 0x10217e000 | 0x700030000 | 2× |

The addresses in the shader-load packets are **exactly** the ISA buffers' IOVAs.

**And the kernel sees the right content in all three:**

```
VS ISA  (bo[3]) dword 4 = 0x3f0a0b0c     <- the VS magic constant
FS ISA  (bo[4]) dword 0 = 0x3f123456     <- the FS magic constant
VS2 ISA (bo[5]) dword 4 = 0x3f0a0b0c
FS ISA first 8 dwords:
   3f123456 204cc000 | 3e800000 204cc001 | 3f000000 204cc002 | 3f800000 204cc003
```

which is textbook a6xx ISA: two dwords per instruction, the index in the low
half and the opcode in the high half.

**So the submission is correct end to end.** GLSL compiles, the command stream is
field-correct, the nested graph is right, the ring executes, the SQE walks it
and reaches `CP_DRAW`, the stream retires, the page table is installed and
correct (image 270), the SMMU permits writes, the CP writes DDR, **and the
shader programs are delivered to the right addresses with the right content.**

**The driver is doing its job correctly the whole time.** That is precisely what
`CLOCKS-GX-DOMAIN-OFF.md` concluded on 2026-10-12, and this session proves it
independently and completely.

## 2. Method note — a capture bug that cost two images

The `ibdump` content dump must not be the only capture. Two consequences:

- **Eviction:** the dmesg ring buffer lost the early submits. Any conclusion
  drawn from a partial dmesg is untrustworthy. Capture with `dmesg -W` to a file
  (host-side or device-side) so nothing is lost.
- **Cross-run joins:** the DRM fake offset (mmoff) is allocated per DRM file, so
  it is *not* stable in general between processes; the allocation order happened
  to be stable for the ISA buffers and not for others (the 20480-byte VSC
  buffer was 0x102189000 in one run and 0x102187000 in another). **Join the
  views within a single run**, which is what `vmascan9.py` does by reading the
  kernel's own debugfs BO table from inside the Mesa process.

## 3. What the 20480-byte buffer actually is — another correction

I read it as "the program arena the GPU is told to run from, and it is empty".
**Wrong.** It is `vsc_draw_strm` — the **visibility stream** buffer:

```c
/* fd6_gmem.c:396 */
#define VSC_DRAW_STRM_SIZE(pitch) ((pitch)*32 + 0x100)

OUT_REG(ring, A6XX_VSC_BIN_SIZE(...),
        A6XX_VSC_DRAW_STRM_SIZE_ADDRESS(.bo = fd6_ctx->vsc_draw_strm,
                                        .bo_offset = 32 * fd6_ctx->vsc_draw_strm_pitch));
```

It is `FD_BO_NOMAP` and **written by the GPU**, not by the CPU — so zeros in it
are expected, not a defect. (The `48b4c08a` / `48b4ca86` entries are
`SP_PS_2D_SRC_INFO` / `SP_PS_2D_SRC_FLAGS` — a 2D blit, i.e. the clear.)

## 4. The live platform state

```
gpll0                            5  5  600000000   ON
  gpll0_out_even                 2  2  300000000   ON
    gcc_gpu_gpll0_div_clk_src    1  1  300000000   ON
      gpu_cc_gmu_clk_src         2  2  200000000   ON
        gpu_cc_gx_gmu_clk        1  1  200000000   ON   <- GX domain is ON now
gcc_gpu_gpll0_clk_src            0  0  600000000   OFF
```

`gpu_cc_gx_gmu_clk` is enabled *now*, after the bring-up chain — so the 2026-10-12
observation (GX off) was taken in a different device state, or something since
has changed. `gcc_gpu_gpll0_clk_src` (the GPU core PLL source) is still off.

## 5. The concrete next experiment: `a6xx.force_gx_clk=1`

The cmdline is truncated at 512 bytes. The untruncated intent
(`.opencode/cmdline242_full.txt`, 535 bytes) ends with:

```
a6xx.force_gx_clk=0 a6xx.send_acd=1 a6xx.uche_gbif_rw=1
```

so the kernel actually sees:

```
... a6xx.force_gx_clk=0 a6xx.send_ac androidboot.verifiedbootstate=...
```

- `a6xx.uche_gbif_rw=1` — **lost** (worked around in image 268 by a 1:1 swap of
  `androidboot.memcg=1`).
- `a6xx.send_acd=1` — mangled to an unknown token, but the parameter's default
  is `true`, so no loss.
- **`a6xx.force_gx_clk=0` — the project's own GX-clock diagnostic — has never
  been enabled.**

`a6xx_gmu.c:2394` implements it: it `devm_clk_get()`s `gx_gmu_clk`,
`clk_prepare_enable()`s it, and prints the one datum nobody has ever read:

```
GXCLK229: clk_prepare_enable(gpu_cc_gx_gmu_clk) ret=%d rate=%lu
GXCLK229: +20ms SPTPRAC=0x%x GX_GDSCR=0x%x TCS0=0x%x RSC0=0x%x RPMHPWR=0x%x gx_is_on=%d
```

`GX_GDSCR` is the **GX power-domain (GDSC) state** and `gx_is_on` the driver's
own view of it — exactly the "confirm the GPU's GDSC/power state" step that
`CLOCKS-GX-DOMAIN-OFF.md` listed as next-move 1 and that was never executed.

**Image 273 = image 272's kernel with ONE character changed in the cmdline**
(`a6xx.force_gx_clk=0` → `=1`). Same length, same position, no truncation
change, no kernel change. The cheapest remaining experiment, and it is the
project's own.

## 6. Also worth doing once, in the same cmdline

`CLOCKS-GX-DOMAIN-OFF.md` says the DT's `qcom,kgsl-3d0` node requests only
`GPU_CC_CX_GMU_CLK`; `GPU_CC_GX_GMU_CLK` is normally opened by **KGSL** via the
GMU/RPMH power-up path, and we run msm/drm instead. The tree's comment says the
DT change ("add GPU_CC_GX_GMU_CLK to the GMU node (done in sm8150-gpu.dtsi)")
is already in — which is consistent with `gpu_cc_gx_gmu_clk` now reading ON.

If `force_gx_clk=1` shows `gx_is_on=0` and `GX_GDSCR=0`, the next step is the
`qcom,gpu-quirk-cx-gdsc` quirk, which KGSL has workaround code for and this
driver does not.
