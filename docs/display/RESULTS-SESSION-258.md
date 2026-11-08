# Session 258 — consolidated results

Device: Galaxy Note10+ (d2q), Adreno 640, CAF msm DRM 4.14.190, Mesa 22.3.6
freedreno (`FD640`). Kernel image in use: `258-halium-boot-a6xx-boflags.img`
(`999fb8eb…`). GPU health confirmed throughout on every run.

**Status: still no non-black pixel, but the problem is now narrow and the
kernel is exonerated by direct measurement.**

---

## 1. Evidence table — everything established this session

| # | Claim | Method | Result |
|---|---|---|---|
| 1 | Kernel upload + submit path is sound | `rawdrm.py` — GEM_NEW + GET_OFFSET + mmap + memcpy + munmap + mmap, no Mesa | pattern round-trips |
| 2 | Userspace writes reach **submitted** BOs | `fullpath.py` — same, then `DRM_IOCTL_MSM_GEM_SUBMIT`, checked in `a6xx_submit()` dump | **1024/1024 intact** |
| 3 | Short writes are not lost | `partial.py` — 128 B into a 4096 B WC BO, submitted | **32/32 visible** |
| 4 | 2048 B / 1024 B writes | `partial.py` HALF / QUARTER | **512/512, 256/256** |
| 5 | `glBufferData` upload works | `alias3.py` — vertex buffer, read back via `glMapBufferRange(GL_MAP_READ_BIT)` | pattern byte-for-byte |
| 6 | `glGetBufferSubData` is **unsound** | `alias3.py`, two read routes side by side | returns zeros; direct CPU map shows the data |
| 7 | FS ISA is tiny | `IR3_SHADER_DEBUG=disasm,nocache` | `FRAG prog 1/1: 5 instr … 32 dwords` = **128 B** |
| 8 | `bo[1]`=20480 is not the FS ISA | `bigshader600/1500.py` (N=120→600→1500); `clearonly.py clear` (no draw) | BO set unchanged; present with no draw at all |
| 9 | The draw **is** emitted | `clearonly.py clear` vs `draw` | cmdstream 240 → 244 nonzero dwords |
| 10 | All `FD_MESA_DEBUG` variants negative | `noblit`, `noubwc`, `sysmem,noubwc`, `flush,inorder,dclear,ddraw`, `nobin` | all `[0,0,0,0]` |
| 11 | UBWC static registers correct | source read vs mainline expectations | `0x14 / 0x04 / 0x04 / 0x400000`, `GMEM_RANGE=0x100000+gmem-1` — image 237 already did this |
| 12 | BO names unreachable | Mesa source | `fd_bo_new`/`fd_bo_set_name` wrap naming in `#ifndef NDEBUG`; release Mesa never calls `MSM_INFO_SET_NAME` |
| 13 | Version handshake is fine | `verprobe2.py`, ioctl `0xC0406400` | major 1, minor 4 = `FD_VERSION_SOFTPIN` |
| 14 | GPU healthy | `a6xx_gpu_cpstate` | `SPTPRAC_PWR_CLK_STATUS 0x0`, `RPMH_POWER_STATE 0x0`, `clocking 1`, delta ~676000 |
| 15 | **Cache mode closed by real A/B** | `bo_force_cached=1` (built in image 258) | boflags `0x20000`→`0x10000`, `PIXEL` unchanged `[0,0,0,0]`; `bo[11]` byte-identical (244) both ways |
| 16 | **Phase A already answered, negatively** | `oracle.py` (cache-sound) + `IMAGE-242-RESULTS.md` | CPU never writes; GPU clears; fresh `PROT_READ` mapping reads zero → render engine cannot write DDR |
| 17 | Render target readable kernel-side | BOS dump analysis | no opaque-blue dword (`0xFF0000FF`) anywhere in any submitted BO |
| 18 | `dma_buf_ops` absent | `gbm_bo_get_fd` returns **-1** | this tree defines no `struct dma_buf_ops`, so GEM BOs cannot be exported/mmap'd by userspace |

### 1a. The `bo_force_cached` A/B (item 15) — cache mode closed

The parameter built into image 258 had never been run, because the cache
hypothesis had been refuted before DeepSeek's plan explained *why* every BO is
writecombine. It has now been run as a genuine differential:

| | `boflags` | `PIXEL` |
|---|---|---|
| baseline (`bo_force_cached=0`) | `0x20000` / `0x20002` (WC) | `[0,0,0,0]` |
| forced (`bo_force_cached=1`) | `0x10000` / `0x10002` (CACHED) | `[0,0,0,0]` |

Every BO really did switch to `MSM_BO_CACHED`, so the instrumentation is valid
and the A/B is real. Contents are equivalent: the final command-stream BO is
byte-identical (244 nonzero dwords) in both, and the ISA BOs `bo[1]`/`bo[2]`
are zero either way. Parameter reset to 0.

**Why everything is WC — the version gate (`freedreno_drmif.h:143`,
`msm_device.c:72`, `freedreno_bo.c:105`):**

```c
FD_VERSION_CACHED_COHERENT = 8;                      /* the kernel advertises 4 */
if (version->version_minor >= FD_VERSION_CACHED_COHERENT) { ...test alloc... }
if ((flags & FD_BO_CACHED_COHERENT) && !dev->has_cached_coherent)
   flags &= ~FD_BO_CACHED_COHERENT;                  /* freedreno_bo.c:105 */
```

Mesa never even attempts the `MSM_BO_CACHED_COHERENT` test allocation, so
`has_cached_coherent` stays false and **every** buffer — including command
streams that explicitly request cached-coherent at `freedreno_bo.c:153` — is
demoted to `MSM_BO_WC`. That is the complete cause of image 258's headline
measurement.

Do **not** try to fix this by bumping `MSM_VERSION_MINOR` to 8: the ladder is
monotonic, so 8 also enables `FD_VERSION_ROBUSTNESS` (5) and
`FD_VERSION_SUSPENDS` (7), i.e. per-fault page tables we have never validated.
The module parameter is the safe route and it is now measured.

### 1b. Phase A is already answered (item 16)

`oracle.py` is exactly the cache-sound oracle the plan proposes building, and
`IMAGE-242-RESULTS.md` records its result: the CPU never writes the buffer, the
GPU clears it, the CPU maps it for the first time `PROT_READ`-only, and reads
zero, with a Test C self-check proving the read path works. So
**the render write engine cannot write DDR**, and we are in Phase B2
(driver-side), not B1.

Item 17 repeats that conclusion through the kernel's own BOS dump: no opaque-blue
dword appears in any submitted buffer.

## 1c. Blocked: a sound blit oracle cannot be built on this tree

`oracle2.py` was written to separate RB from SP by letting **Mesa** generate the
blit packets (deliberately not hand-crafted PM4) while making the CPU prefill
sound via `msync(MS_SYNC)` + `munmap`, so the pattern is genuinely in DDR and
read back through a fresh `PROT_READ` mapping.

It is blocked: `gbm_bo_create` succeeds (format `ARGB8888`) but
**`gbm_bo_get_fd` returns -1**, because this tree defines no
`struct dma_buf_ops` — so GEM BOs cannot be exported and userspace cannot mmap
one. The original `IMAGE-239` prefill test could only "work" through this broken
export path, which is consistent with its verdict having been unsound.

An attempt to drive the gbm/EGL window-surface path anyway **wedged the device**
(third wedge this session). Do not repeat it without first solving the export
problem.

**Consequence:** any oracle that needs the CPU to prefill a *Mesa-allocated*
buffer is unavailable on this tree. Kernel-side reads (the BOS dump) remain
sound and are the way forward.

## 2. The shape of the remaining bug

Mesa compiles 5 fragment instructions (128 B), rounds the BO up to 4096, and
uploads into a `FD_BO_NOMAP` buffer (`boflags=0x20000`, writecombine, no
`GPU_READONLY`). The kernel's own submit-time dump shows that buffer as
**0/1024 nonzero** — the head included.

Doing the identical operation from ctypes (rows 2–4) lands perfectly, including
the exact 128-byte-into-4096 shape. So the difference between "works" and
"doesn't" is **not** the kernel, **not** the write size, **not** cache mode, and
**not** the shader compiler.

It is confined to what the ctests never exercise: **freedreno's BO cache and
object identity** (the ctypes path always allocates a fresh BO and never
recycles one), or page state between upload and submit.

Flag-separated BO classes in the failing submit:

```
bo[0] 0x20000  4096    1 nonzero   NOMAP class
bo[1] 0x20000  20480   0 nonzero   NOMAP class, fixed size, NOT a shader ISA
bo[2] 0x20000  4096    0 nonzero   NOMAP class   <-- best FS ISA candidate
bo[4] 0x20000  4096    1 nonzero   NOMAP class
bo[3] 0x20002  4096  72/149/244    ring command stream
```

## 3. Next step (no rebuild required)

In the Mesa process, after the draw, walk `/proc/self/maps` for `MAP_SHARED`
VMAs on the DRM fd, read the first 64 bytes of each, and compare against the 32
dwords of ISA that `IR3_SHADER_DEBUG=disasm` reports.

- A VMA holds the ISA → the write landed in userspace; the loss is kernel-side
  at submit time.
- No VMA holds it → the memcpy never reached the mapped pages; look at freedreno
  BO cache reuse and `bo_map`.

Deliberately **not** attempted: decoding `CP_LOAD_STATE6` from the dumped
stream. Two attempts to guess the PM4 header bit layout were both wrong, which
is the exact mechanism behind every false claim tonight.

## 4. Harnesses (preserved in `harnesses/`, also in `/home/usb/.opencode-tmp/`)

| file | purpose |
|---|---|
| `rawdrm.py` | raw DRM upload round-trip, no Mesa |
| `fullpath.py` | raw DRM upload **+ submit**, kernel-dump cross-check |
| `partial.py` | short-write tests (128/1024/2048/4096 B) |
| `alias2.py` | UBO probe with a submit self-check (self-check correctly failed) |
| `alias3.py` | vertex-buffer probe, two read routes |
| `clearonly.py` | `clear` vs `draw` A/B — the draw-emission test |
| `bigshader600.py`, `bigshader1500.py` | shader-size scaling (N=600, 1500) |

## 5. Standing operating rules (learned the hard way)

1. **The GPU does not come up on its own.** Every boot:
   `echo on > …/kgsl-3d0/power/control` then
   `echo 1 > /sys/devices/platform/soc/ae00000.qcom,mdss_mdp/a6xx_gpu_load`.
   Without it `priv->gpu` is NULL, `get_param` returns -ENXIO, and Mesa falls
   back to llvmpipe — which renders a perfect `glClear`. A run reported
   `PIXEL=[0,0,255,255]` on llvmpipe and it looked like the breakthrough.
2. **Never trust a pixel without `renderer` containing `FD640`.** Only 6 of ~100
   harnesses had that guard; one has now been added to `bigshader.py`.
3. **`glGetBufferSubData` is not a readback here** — it is a GPU-side copy and
   returns zeros when the GPU cannot write. Use `glMapBufferRange(GL_MAP_READ_BIT)`.
4. **Health via `a6xx_gpu_cpstate`**, not `runtime_status`
   (`suspending` is a red herring). Check before *and* after every GPU action.
   Two wedges this session, each needing a manual reboot.
5. **Any harness reading back kernel memory must first prove its buffer reached
   `bos[]`.** Two of tonight's retracted claims were null tests.
6. `/tmp/opencode` is **not** durable — harnesses now live in
   `/home/usb/artifacts/gpu-bringup/harnesses/` and `/home/usb/.opencode-tmp/`.

## 6. Retraction ledger

| document | claim | status |
|---|---|---|
| `ROOT-CAUSE-USERSPACE-WRITES-INVISIBLE.md` | userspace writes invisible to kernel | **RETRACTED** — null test; buffers never submitted |
| `CORRECTION-PATTERN-TEST-WAS-A-NULL-TEST.md` | why that test was void | stands |
| `ALIAS3-RESULTS.md` | "BO IS ZERO — upload never landed" | **WRONG** — `glGetBufferSubData` unsound |
| `FINDINGS-KERNEL-EXONERATED-ISA-NOT-SUBMITTED.md` §4 | "the draw is never emitted" | **RETRACTED** — it is emitted |
| `IMAGE-258-BOFLAGS-NAMED-BOS.md` | cache-mode hypothesis | **REFUTED** — all BOs are `MSM_BO_WC` |
| subagent mechanism 1 | writecombine store visibility | **REFUTED** — by rows 2–4 |
| subagent mechanism 2 | truncated disk-cache blob | **excluded** — no ISA BO allocated at all |

## 7. Image 258 — what it contributed

Three diagnostic changes, none altering GPU behaviour at defaults:

1. BOS dump prints `bo->flags` instead of `submit->bos[b].flags` (the latter is
   internal CAF submit bookkeeping — `BO_VALID|BO_LOCKED|…`, decoded `0x6003` —
   and **never** a cache mode). This is what let the cache hypothesis be
   finally refuted rather than assumed.
2. `MSM_INFO_SET_NAME` now stores the name. Correct, but unreachable: release
   Mesa compiles the naming path out under `NDEBUG`.
3. `bo_force_cached` module parameter, default off, for a follow-up A/B.

Header diffed against the 242 reference: 23 bytes differing, all inside the
sha1 id field, so the only functional change is the kernel.

## 8. Flush-path probe: `glFlush` loop vs true `glFinish` vs `eglSwapBuffers`

Harness: `harnesses/flushpath_probe.py` (same clear+draw workload, GBM window
surface, `FD640`-gated, no `glReadPixels`). Each mode ran as a fresh process
with `dmesg -C` + `ibdump=1`, then the kernel log was captured.

| mode | userspace result | kernel BOS dump |
|---|---|---|
| 20x true `glFlush()` | returned, `gl_err=0x0000` | grew: distinct `BOS[seq=1]` lines present |
| 1x true `glFinish()` | **returned**, `gl_err=0x0000` | grew: distinct `BOS[seq=4]` lines present |
| 1x `eglSwapBuffers()` | returned 1, `egl_err=0x3000`, `gl_err=0x0000` | grew: distinct `BOS[seq=2]` **and** `BOS[seq=3]` lines present |

Notes and limits:

- True `glFinish()` did **not** block here; it returned with no GL error and the
  GPU stayed healthy (`SPTPRAC_PWR_CLK_STATUS 0x00000000`, `RPMH_POWER_STATE
  0x00000000`, `clocking 1` before/after). This contradicts the older blanket
  "true `glFinish` blocks forever" note for this workload.
- The per-dword BOS dump floods `dmesg`, so early header lines can be lost to
  ring rotation; the reliable signal used here is **distinct `BOS[seq=...]`**
  values, not exact BO counts.
- This does **not** show that `eglSwapBuffers` fixes anything: all three modes
  produced kernel submits. The swap run produced two distinct submit seqs, but
  that extra submit is not yet identified (it may be present/commit work rather
  than the draw).
- This also does not overturn the compute `GEM_SUBMIT: 0 calls` finding: that
  was a surfaceless compute dispatch using `glFlush` (and an old harness that
  aliased `glFinish` to `glFlush`). The draw path demonstrably reaches
  `a6xx_submit()` here.