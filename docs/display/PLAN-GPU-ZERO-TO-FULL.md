# PLAN — from zero rendering acceleration to a full-working Adreno 640 (d2q)

Status: planning document. Nothing in this file has been executed.
Date: 2026-10-15
Supersedes-in-spirit: the "next move" sections scattered across the image
202→258 documents; this file is the forward plan.

Read alongside:
- `RESULTS-SESSION-258.md` (latest consolidated state)
- `FINDINGS-KERNEL-EXONERATED-SHARP-PROBLEM.md`
- `IMAGE-243-PARITY-BUILD.md` (runtime-toggleable CAF parity params)
- `CLEAN-SESSION-RECIPE.md` (boot/health procedure)
- `harnesses/` (ctypes GL/DRM probes)

---

## 1. Where we are

Stack: Samsung ABL → Halium-style `boot.img` → CAF/Samsung `msm` DRM
`4.14.190` ("ShinyKernel-gpu-exp") → Debian 12 arm64 → Mesa 22.3.6
`freedreno`, reported as `FD640`.

### 1.1 Proven working (high confidence, multiple methods)

- GPU power-up, GMU boot, HFI (8 msgs / 8 ACKs), `hw_init ret=0`.
- Ring programmed (`CP_RB_CNTL=0x20c`) and *executed* (RPTR chases WPTR).
- Fences signal. `CP_EVENT_WRITE` / `CACHE_FLUSH_TS` timestamps and sentinels
  land in DDR (read back from the CPU).
- No faults anywhere: 0 RBBM faults, 0 SMMU stalls.
- SMMU is translating: 28 GiB IOVA aperture on 11.2 GiB RAM.

### 1.2 Proven broken (high confidence)

- *Every write from the render/shader domain is absent.*
  - no pixels;
  - no compute → SSBO writes;
  - no 2D `CP_BLIT` clear output.
- The backend engages (`RB=1`) and retires; prefilled `0xAA` is untouched.

### 1.3 Evidence-integrity warning

The corpus has **retracted** several load-bearing claims (see
`RESULTS-SESSION-258.md` §6):

- the "pattern test" was a null test (buffers were never submitted);
- `glGetBufferSubData` is unsound here (readback returns zeros);
- the cache-mode hypothesis was **refuted** (all BOs are `MSM_BO_WC`);
- "the draw is never emitted" was **refuted** (the draw *is* emitted);
- the `ROOT-CAUSE-USERSPACE-WRITES-INVISIBLE` root cause was **retracted**.

Therefore Phase A exists specifically to re-establish an oracle that cannot
fool us, before any new root-cause claim is allowed to stand.

---

## 2. Two facts the corpus has not yet exploited

These come from the actual Mesa 22.3.6 source
(`/tmp/opencode/mesafull`, `/tmp/opencode/mesa-22.3.6`) and the vendor KGSL
source (`/home/usb/ShinyKernel/Kernel/drivers/gpu/msm`).

### 2.1 The shader-ISA upload path is fully known — and has a named failure mode

In `src/gallium/drivers/freedreno/ir3/ir3_gallium.c`:

```c
upload_shader_variant():
    v->bo = fd_bo_new(compiler->dev, v->info.size, FD_BO_NOMAP, ...);
    fd_bo_upload(v->bo, v->bin, 0, v->info.size);
```

The MSM backend (`src/freedreno/drm/msm/msm_bo.c`) has **no `.upload` op**, so
`fd_bo_upload()` (`freedreno_bo.c:482`) falls through to:

```c
memcpy((uint8_t *)bo_map(bo) + off, src, len);
```

Note this is the internal `bo_map()`, **not** `fd_bo_map()` — so the
`FD_BO_NOMAP` guard is bypassed. `bo_map()` resolves the mmap offset via:

```
bo->funcs->offset = msm_bo_offset -> bo_allocate
   -> DRM_MSM_GEM_INFO / MSM_INFO_GET_OFFSET
   -> os_mmap(0, size, MAP_SHARED, dev->fd, offset)
```

**If that offset is wrong, the memcpy writes the wrong physical pages and
nothing logs an error.** That is exactly the observed signature: "the ISA BO is
zero at exactly the right size." This converts "BO-cache weirdness in the
abstract" into a specific, testable mechanism.

### 2.2 The a6xx driver here never emits `CP_SET_SECURE_MODE`

Grep of the experimental tree:

```
drivers/gpu/drm/msm/adreno/a5xx_gpu.c:587:  OUT_PKT7(..., CP_SET_SECURE_MODE, 1);
```

There is **no** `CP_SET_SECURE_MODE` in `a6xx_gpu.c` or anywhere else in the
a6xx path. Instead the tree writes `RBBM_SECVID_TRUST_CNTL = 0` directly (the
a5xx "no-zap-available" fallback), while the zap *is* loaded via
`subsys-pil-tz` (PAS 13). Mainline a6xx, by contrast, loads the zap and then
emits `CP_SET_SECURE_MODE 0` from the ring and waits for idle.

This mismatch has never been tested.

### 2.3 A coherency red herring to stop re-chasing

`msm_drv.c` advertises only `MSM_VERSION_MINOR 4`. Mesa's
`freedreno_bo.c:105` therefore demotes every `FD_BO_CACHED_COHERENT` request:

```c
if ((flags & FD_BO_CACHED_COHERENT) && !dev->has_cached_coherent)
   flags &= ~FD_BO_CACHED_COHERENT;
```

and `msm_bo.c:181` maps everything else to `MSM_BO_WC`. This is consistent
with IMAGE-258 ("all BOs are `0x20000`") and **kills the CPU↔GPU
coherency / write-combine store-visibility theory for uploads**: WC is
non-cacheable, so that path cannot produce stale reads. Do not re-open it
without new first-hand measurement.

---

## 3. Phase A — the one experiment that splits the problem in half

Nothing else matters until we know whether the **render write engine can write
DDR at all** when driven by code we fully control. The ctypes path proved the
*CP* can; nobody has proven the *RB/GRAS* can.

### A.1 Kernel-owned blit oracle

From `a6xx_gpu.c` (a debugfs write, or a one-shot hook during bring-up):

1. Allocate a driver BO (fresh, known IOVA).
2. Emit a minimal sysmem `CP_BLIT`:
   - `GRAS_ADDR_MODE_CNTL = 0`
   - `GRAS_2D_DST_TL/BR` covering a known rect
   - `BLIT_OP_SCALE`
   - the `RB_DBG_ECO_CNTL` blit magic (`0x04100000`)
   - `CP_WAIT_FOR_IDLE`
   - `CACHE_FLUSH_TS`
3. Destination = that driver BO's IOVA.
4. CPU-read the destination through the driver vmap.

Interpretation:
- **Writes land** → driver + SMMU + RB write path are sound. The defect is
  entirely in Mesa's command stream or in BO/offset identity → Phase B1.
- **Doesn't land** → defect is in driver init / secure-mode / 64-bit address /
  cache config, independent of Mesa → Phase B2. This is the outcome current
  evidence predicts (the shader-free 2D blit also fails).

### A.2 ctypes blit oracle (cheaper, nearly as decisive)

Do the same `CP_BLIT` from the existing `rawdrm.py` / `ibomini.py` submit path
with a fresh GEM BO. This removes Mesa from the loop, though not userspace.

### A.3 Do both if time allows

Agreement between the kernel oracle and the ctypes oracle removes the "maybe
my userspace is wrong" escape hatch.

### A.4 Guard rails (earned the hard way)

- Gate every result on the real renderer / raw ioctl success, never on a pixel
  alone (`GL_RENDERER` must contain `FD640`).
- Health via `a6xx_gpu_cpstate` before *and* after, never `runtime_status`.
- Pin PM before first GPU use; trigger `a6xx_gpu_load` only after uptime 300 s.
- Launch freeze-prone runs detached with `/root/poller.sh` running.
- `/tmp/opencode` is not durable — write harnesses to durable paths.

---

## 4. Phase B1 — if the blit oracle works (Mesa-side defect)

The problem is the freedreno command stream or BO identity.

1. **Walk `/proc/self/maps` inside the Mesa process** after `glFlush`: dump the
   first 64 B of every `MAP_SHARED` VMA on the DRM fd and compare against the
   32 dwords `IR3_SHADER_DEBUG=disasm,nocache` reports.
   - A VMA holds the ISA → the memcpy landed; loss is at submit/kernel →
     compare the kernel `bos[]` dump for that exact handle.
   - No VMA holds it → the `MSM_INFO_GET_OFFSET` → `mmap` → memcpy path is
     broken for this BO class. Log every `GET_OFFSET` return in the kernel and
     assert offsets are non-zero and unique across submitted BOs; add a
     bounds/contents check immediately after `bo_allocate()`.
2. **Cross-check the offset theory directly in ctypes**: allocate two BOs,
   print both `GET_OFFSET` values, write a distinct pattern into each, and
   verify they are distinct physical memory. Colliding or zero offsets = root
   cause found, and it is a one-line kernel UAPI fix, not a compositor problem.
3. Only after uploads are proven: decode `CP_LOAD_STATE6`. Use the
   XML-generated headers, never memory — two prior guesses at the PM4 header
   layout were both wrong.

---

## 5. Phase B2 — if the blit oracle fails (driver-side defect)

Ranked by "explains *all* the evidence": CP writes land, RB/SP/CC write nothing,
RB retires, no faults.

| Rank | Candidate | Why it fits | Decisive test |
|---|---|---|---|
| 1 | **GPU not taken out of secure mode.** Emit `CP_SET_SECURE_MODE 0` from the ring after zap init (mainline behaviour), instead of the direct `RBBM_SECVID_TRUST_CNTL = 0` write | A whole-domain silent drop (RB/SP/CC) with CP still working is what a secure/trust gate looks like; TZ-set `RBBM_SECVID_TSB_TRUSTED_BASE/SIZE` can cover the aperture | Add the packet post-zap; re-run the Phase-A oracle. One build, no userspace change |
| 2 | **`CP_ME_INIT` divergence.** Ours = mask `0x2f`, 8 dwords. CAF = mask `0x16f`, 11 dwords + `pwrup_reglist` with the `REGISTER_INIT_LIST_WITH_SPINLOCK` ordinal | CAF's list is exactly the per-block `ADDR_MODE_CNTL`, `UCHE_*`, `RB_NC_MODE_CNTL`, `GMEM_RANGE` set a shader-domain write needs | Port CAF `_set_ordinals` + `a6xx_pwrup_reglist` (`KGSL/adreno_a6xx.c:397,701,1048`); re-run oracle |
| 3 | **GPU cache / L2 → DDR coherence.** RB/CC writes sit in UCHE until a flush; only `CACHE_FLUSH_TS` forces them out | Explains "CP `CACHE_FLUSH_TS` lands, RB writes do not" cleanly | After the oracle blit, emit explicit `CACHE_FLUSH_TS` / `CACHE_INVALIDATE` and re-read; dump `UCHE_*`, `GBIF_SCACHE_CNTL0` vs mainline. (IMAGE-242 refuted LLCC/SCID specifically, not the general UCHE-flush question) |
| 4 | **64-bit render-target address truncation.** 28 GiB IOVA aperture on 11.2 GiB RAM means high bits matter | A truncated RT address silently writes a different, mapped BO → RB retires, no fault, target untouched | Force the oracle destination IOVA above 4 GiB (or dump `GRAS_2D_DST_BASE` / `RB_*_BASE` hi-halves vs IOVA); repeat with a low IOVA |
| 5 | **sysmem vs GMEM target interpretation** | Makes the RB write on-chip GMEM and never resolve | Partly tested (UBWC off, `noubwc`); add an explicit sysmem-mode assertion in the oracle |

Keep the IMAGE-243 parity module params (`a640_roq2`, `a640_gbif_halt`,
`a640_gmu_cgc`, `a640_qsb`, `a640_rbbm_nc`) as runtime A/B knobs, but note
their results were never consolidated.

### 5.1 Retire permanently (do not re-open without new measurement)

dma-iopmu identity, `CP_SMMU_TABLE_UPDATE`, LLCC/SCID, amsbc, UB64/UBWC config,
unclocked blocks, "zap-loading-is-insufficient", CPU↔GPU coherency (killed by
the WC demotion in §2.3).

---

## 6. Phase C/D — from first pixel to a working GPU

```
Phase A  Bulletproof oracle                    (hours)  → CP_BLIT writes Y/N
Phase B  Fork the search:
   B1 (oracle works) → Mesa/offset hunt                 → ISA & RT visible at submit
   B2 (oracle fails) → secure-mode → CP_ME_INIT → UCHE flush → addr64
Phase C  First non-black FD640 pixel           (milestone) → then a real GL workload
Phase D  Present / composite: kmsro over simpledrm, or fix MDSS/DPU scanout
Phase E  Robustness: cold-boot without manual PM pin (fold the
         power-control + a6xx_gpu_load dance into boot), suspend/resume, fences
```

**Definition of done for "full-working GPU":**

- `glClear`/triangle render *and* `glMapBufferRange` readback agree;
- a real app (glmark2 or a KWin greeter) renders on `FD640` at a sane frame rate;
- output is visible on screen through *some* scanout path (firmware `simpledrm`
  + kmsro is acceptable and is the proven route);
- it survives a cold reboot with no manual incantation.

---

## 7. Phase 4 alternative — the robust endgame (recommended parallel track)

All of §3–§6 is an attempt to bend a heavily diverged Android 4.14 DRM fork to
serve upstream Mesa — a fork whose UAPI even disagrees with Mesa on BO cache
flags, and whose a6xx init is missing mainline's secure-mode step. That is a
long, brittle road to "full-working GPU."

**SM8150 / Adreno 640 with freedreno is already proven on mainline**, in exactly
the render-only topology we need:

- `Bentlybro/gts6l-mainline-linux` → `docs/GPU.md`: Samsung SM8150 (Tab S6),
  Adreno 640 + GMU, mainline kernel, `msm`/a6xx as a **render node**
  (`card1`/`renderD128`) with scanout left on the firmware `simpledrm`
  framebuffer; Mesa `freedreno` + **kmsro**; KWin Wayland composites on
  `renderD128`. This is the target architecture.
- Kernel source: `sm8150-mainline/linux` / `sm8150-linux-mainline/linux`
  (branch `linux-6.12_rc7`).
- Debian userspace already has Mesa 22.3.6, which supports `kmsro`.
- Boot caveat: Samsung ABL refuses a vanilla mainline DTB (it merges its own
  overlays and corrupts the tree — documented for SM-X800). The proven route is
  **Project Aloha / MU-SM8150Pkg UEFI**, which supports SM8150 Samsung targets
  (`gts6l`, `gts6lwifi`, `beyond1qlte`, `winner`). **d2q is not listed**, but it
  is a mechanical port: copy an SM8150 Samsung target, drop in `Binaries` from
  d2q's `xbl`, and provide `android-d2q.dtb` + `linux-d2q.dtb`.

**Concrete spike (time-boxed, independent of the CAF track):**

1. Stand up Project Aloha UEFI for a stock SM8150 Samsung target; confirm it
   boots on d2q (fastboot `boot` only — flash nothing).
2. Boot an `sm8150-mainline` image with `linux-d2q.dtb` and a minimal
   kmsro-capable initramfs; confirm `simpledrm` + a `renderD128` appears.
3. Run the *existing* Mesa 22.3.6 `FD640` harness there. If pixels appear where
   258 images of CAF work produced none, pivot.
4. Keep the Debian 12 rootfs; only the boot chain + kernel change.

Cost/benefit: one UEFI port + one DTS, versus an unbounded bug hunt in a fork.
It also provides a *reference GPU implementation* to diff against if the CAF
tree is later revisited.

---

## 8. Standing operating rules (carry forward)

1. The GPU does not come up on its own. Every boot:
   `echo on > /sys/devices/platform/soc/2c00000.qcom,kgsl-3d0/power/control`
   then `echo 1 > /sys/devices/platform/soc/ae00000.qcom,mdss_mdp/a6xx_gpu_load`
   (never before uptime 300 s). Without it Mesa falls back to llvmpipe, which
   renders a perfect `glClear`.
2. Never trust a pixel without `GL_RENDERER` containing `FD640`.
3. `glGetBufferSubData` is not a readback here; use
   `glMapBufferRange(GL_MAP_READ_BIT)`.
4. Health via `a6xx_gpu_cpstate` (`SPTPRAC_PWR_CLK_STATUS` / `RPMH_POWER_STATE`
   = 0, `CP_TIMESTAMP clocking 1`), *not* `runtime_status`. Check before and
   after every GPU action.
5. Any harness reading back kernel memory must first prove its buffer reached
   `bos[]`.
6. Harnesses and notes belong in durable paths, not `/tmp/opencode`.
7. Reboot via `echo b > /proc/sysrq-trigger`; launch freeze-prone runs detached
   with `/root/poller.sh` running.
