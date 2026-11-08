# Image 270 — Results: the page-table prologue is NOT the defect

Date: 2026-10-18
Image: `artifacts/flash/270-halium-boot-a6xx-ptprologue.img`
sha256 `d4085673e4bdaeec3de126e5bc6f9f9b9e88480ae8fdabcd3fdffcaa1831e726`
Base: image 263. Single variable: the kernel.

## Verdict

**Hypothesis REFUTED.** Upstream's missing page-table prologue
(`CP_SMMU_TABLE_UPDATE` with TTBR0+ASID) is **not** what stops the GPU writing
to memory. The prologue was emitted on every submit, field-correct, and the
render and compute write paths are bit-for-bit identical to the image-267
baseline.

This eliminates the strongest remaining lead from the submission-path diff
(`FINDING-SUBMIT-PATH-MISSING-PAGETABLE-PROLOGUE.md`).

## The test was valid — falsifiability ladder

| level | evidence | result |
|---|---|---|
| 1 | `PTU270` string present in the packed kernel payload | **PASS** (host, pre-flash) |
| 2 | n/a — the value comes from the SMMU, not a cmdline token | — |
| 3 | **decisive**: driver consumed TTBR0 and emitted the prologue | **PASS** — 5/5 submits |
| 4 | workload result | no change |

```
[   67.904919] PTU270[seq=1] ttbr0=0x10003e973d000 asid=1
[   68.908925] PTU270[seq=2] ttbr0=0x10003e973d000 asid=1
[   68.910955] PTU270[seq=3] ttbr0=0x10003e973d000 asid=1
[   68.912546] PTU270[seq=4] ttbr0=0x10003e973d000 asid=1
[   68.913101] PTU270[seq=5] ttbr0=0x10003e973d000 asid=1
```

Not the `no pagetable params, prologue skipped` branch. A stable, non-zero page
table base was obtained and used on every submit.

### The emitted packet is field-correct

PM4 spec (`regs/mesa-pm4.xml`, domain `CP_SMMU_TABLE_UPDATE`):

```xml
<reg32 offset="0"><bitfield name="TTBR0_LO" low="0"  high="31"/></reg32>
<reg32 offset="1"><bitfield name="TTBR0_HI" low="0"  high="15"/>
                   <bitfield name="ASID"     low="16" high="31"/></reg32>
<reg32 offset="2"><bitfield name="CONTEXTIDR" low="0" high="31"/></reg32>
<reg32 offset="3"><bitfield name="CONTEXTBANK" low="0" high="31"/></reg32>
```

Base in bits [47:0], ASID in bits [63:48] — the AArch64 `TTBR0` layout, the
same one `arm-smmu` uses (`TTBRn_ASID_SHIFT = 48`, `arm-smmu.c:3627`).

| dword | emitted | meaning |
|---|---|---|
| 0 | `0xe973d000` | TTBR0_LO, base[31:0] |
| 1 | `0x00010003` | TTBR0_HI = `0x0003` (base[47:32]), ASID = `0x0001` at [31:16] |
| 2 | `0x00000000` | CONTEXTIDR |
| 3 | `0x00000000` | CONTEXTBANK |

The hypothesis was therefore genuinely tested, not merely exercised.

## Workload result — unchanged from the image-267 baseline

Renderer was **`FD640`** (real GPU, not llvmpipe — see the trap below).

| test | image 267 baseline | image 270 |
|---|---|---|
| T1 compute → SSBO | prefill intact `256/256`, pattern `0/64` | prefill intact `256/256`, pattern `0/64` |
| T2 transform feedback → TBO | prefill intact `1024/1024`, records `0/64` | prefill intact `1024/1024`, records `0/64` |
| T3 fragment → FBO | `[0,0,0,0]` | `[0,0,0,0]` |

GPU healthy before and after (`SPTPRAC_PWR_CLK_STATUS 0x0`, `RPMH_POWER_STATE
0x0`, `clocking 1`, clock delta advanced 675990 → 676152, i.e. real work driven).

## The llvmpipe trap, hit again and caught

The first run of the session reported:

```
T1  VERDICT: GPU WROTE BUFFER MEMORY (compute->SSBO alive)
```

with `prefill bytes still 0xAA = 0/256` and `64/64` pattern matches. **That was
llvmpipe**, the CPU software rasteriser, writing the buffer:

```
MESA: error: get_param:232: get-param failed! -6 (No such device or address)
MESA: error: fd_pipe_new2:52: allocation failed
renderer=b'llvmpipe (LLVM 15.0.6, 128 bits)'
```

The GPU had not been brought up. It is discarded. `wrt2.py` has **no `FD640`
guard** and must not be trusted without reading the renderer string — same
defect that once produced a fake "first non-black pixel" in this project
(`PIXEL=[0,0,255,255]` on llvmpipe, image 258).

**Add a renderer guard to `wrt2.py`** — it should abort unless
`FD640` is in `glGetString(GL_RENDERER)`. It is the single most dangerous
harness in the set because it is the oracle for every conclusion in this
project.

## What this eliminates, and what it leaves

Eliminated:
- Missing upstream page-table prologue (`a6xx_set_pagetable()`)

Left standing:
- The defect is shared by the RB (render) and SP (compute) write paths.
- Mesa's state is live and workload-tracking in the aperture registers (267).
- The CP writes DDR (`memptrs->fence` increments) — so the CP and the DDR path
  work; only *shader* stores are dead.
- SMMU writes are permitted (GEM mapped `IOMMU_READ|IOMMU_WRITE`).
- GBIF `UCHE_GBIF_GX_CONFIG` is not the gate (268, falsifiable negative).
- A GPU page table now *is* installed and is the right one, and the shader
  still writes nothing — so the failure is **downstream of translation**.

That last point is a real advance in the model: as of 270 the GPU has a valid
page table, a permitted SMMU, live Mesa state, and a healthy CP, and the
shader still writes nothing. The remaining suspects are things that sit between
"shader executes" and "store reaches DDR": the store/flush path, the
aperture/UBWC decode, a coherency or cache-flush barrier, or a secure/non-secure
gate.

## Evidence

- `artifacts/gpu-bringup/evidence-270/RESULTS-270-TRANSCRIBED.md` — the live
  console capture (transcribed: the device froze before the files could be
  copied off, and `/userdata` does not survive the unclean reboot)
- `artifacts/gpu-bringup/patches/270-pagetable-prologue.patch` — the change
- `artifacts/gpu-bringup/IMAGE-270-PAGETABLE-PROLOGUE.md` — the pre-flight scope

## Operational notes learned this session

1. **Flash command** — the working invocation is plain
   `heimdall flash --BOOT <img>`, **no `--no-reboot`**. Heimdall performs the
   reboot itself. Using `--no-reboot` followed by `heimdall reboot` left the
   device stuck in download mode.
2. **GPU bring-up is mandatory** and must precede every harness run:
   ```sh
   bash /home/usb/.opencode/u.sh --fix
   echo on > /sys/devices/platform/soc/2c00000.qcom,kgsl-3d0/power/control
   nohup /root/poller.sh >/dev/null 2>&1 &
   echo 1  > /sys/devices/platform/soc/ae00000.qcom,mdss_mdp/a6xx_gpu_load
   ```
   Health check, before *and* after any GPU work:
   ```sh
   C=/sys/devices/platform/soc/ae00000.qcom,mdss_mdp/a6xx_gpu_cpstate
   grep -E 'SPTPRAC_PWR_CLK_STATUS|RPMH_POWER_STATE' $C   # both 0x00000000
   grep 'CP_TIMESTAMP' $C                                 # "clocking 1"
   ```
3. **Storage** — `/` is `/dev/loop0`, 2.0 GB, **100 % full, 0 bytes free**.
   `/userdata` (`/dev/sda30`) has 224 GB. Put all harness output there.
   `/userdata/<dir>` does **not** survive a reboot — recreate it every session.
4. **Evidence capture** — `/sys/fs/pstore` is empty and there is no
   `/dev/ramoops`; the cmdline's `ramoops.*` settings do not produce a
   reachable ramoops backend on this build. `/proc/last_kmsg` is the
   **UEFI/bootloader** log, not the kernel log. Kernel evidence must be
   captured over the live SSH link, in the same boot, immediately.
