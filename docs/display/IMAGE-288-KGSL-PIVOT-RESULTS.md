# IMAGE 288 — the KGSL pivot: KGSL runs on the Debian boot

Date: 2026-10-23
Plan: `PLAN-GPU-TRANSITION-2026-10-23.md` §B (Track B)
Image: `artifacts/flash/288-halium-boot-kgsl-debian.img`
sha256 `6e35aa5dedf40051bbf62eaa8a6082c3811f60243b02432a6ae46726b5738c45`

## What changed

One config flip and four `#ifdef` guards — no functional code changes:

1. `configs/d2q_a6xx_drm_experimental.config`: the two
   `# CONFIG_QCOM_KGSL is not set` lines removed (backup:
   `d2q_a6xx_drm_experimental.config.orig-287`).  The baseline's
   `CONFIG_QCOM_KGSL=y` + `_IOMMU=y` therefore win, and the in-tree KGSL
   driver — byte-identical to the pristine ShinyKernel copy (verified with
   `diff -rq`) — binds to `2c00000.qcom,kgsl-3d0` again.
2. `drivers/gpu/drm/msm/{msm_drv.c,msm_gpu.c,msm_debugfs.c}`: the image-185-287
   instruments (a6xx_gpu_load, a6xx_gpu_cpstate, gmu_log, gmu_boot_error,
   deferred load thread) guarded with `#if !defined(CONFIG_QCOM_KGSL)`;
   the pristine CAF stubs (`load_gpu` no-op, `add_gpu_components` no-op,
   empty `adreno_register/unregister`) restored — matching
   `ShinyKernel/Kernel/.../msm_drv.c`'s original shape.
3. The drm/msm adreno objects are excluded automatically by the Makefile's
   `ifneq ($(CONFIG_QCOM_KGSL),y)` gate: DRM builds display-only.

Cmdline and ramdisk are byte-identical to image 285 (the same packer; only
the kernel payload swapped).  The `a6xx.*` module params on the cmdline are
inert noise in this image.

## Kernel-side verification (first boot, uptime 79 s → PASS)

```
gpu_model = Adreno640v2
gpuclk = 257000000          min/max = 257/585 MHz
/dev/kgsl-3d0 opens; IOCTL_KGSL_DEVICE_GETPROPERTY(DEVICE_INFO):
  device_id = 0x00000001
  chip_id   = 0x06040001   <- Adreno 640
  gmem_size = 0x100000     <- 1 MB (correct)
```

Zap shader loads through PAS on first open:

```
subsys-pil-tz soc:qcom,kgsl-hyp: a640_zap: loading from 0xf8a00000
subsys-pil-tz soc:qcom,kgsl-hyp: a640_zap: Brought out of reset
pil-a640_zap: active_count(1)
```

No drm a6xx instruments exist in this image (verified: no `a6xx_*` sysfs
nodes under mdss_mdp) — the DRM side is display-only, as designed.

**The 300 s rule, the PM pinning and the a6xx_gpu_load trigger are GONE.**
Those were drm/msm bring-up instruments; KGSL attaches on first open of
`/dev/kgsl-3d0` (observed: zap load at t=82 s, triggered by kgslinfo.py) and
manages its own power.  First GPU use now requires no manual steps.

## The GL stack: what works and what had to be rebuilt

- Debian 12's stock Mesa 22.3 has **no kgsl support at all** (the kgsl
  KMD backend postdates it).
- **Mesa 25.3's kgsl backend is Turnip-only** (`src/freedreno/vulkan/tu_knl_kgsl.cc`;
  the old Gallium-GL kgsl path from `src/freedreno/drm/kgsl/` was dropped in
  the 24.x KNL refactor).  A freedreno-GL-over-kgsl build is therefore NOT
  available from stock 25.3 — the first Mesa build (gallium freedreno,
  kgsl-only) produced a libEGL that could only try the DRM/DRI2 path
  (`fd_pipe_new2: allocation failed`, fd -1) and failed at eglInitialize.
- Correct stack (the Android-community standard): **Turnip (Vulkan) over
  KGSL + Zink (GL-on-Vulkan)**.  Built in a Debian 12 arm64 qemu-user chroot
  on mnt-sda (`/home/usb/mnt-sda/mesa-root`), so the produced binaries link
  against the phone's exact glibc:
  `meson -Dgallium-drivers=zink -Dvulkan-drivers=freedreno
  -Dfreedreno-kmds=kgsl -Dplatforms= -Degl=enabled -Dgles1=enabled
  -Dgles2=enabled -Dglx=disabled -Dgbm=disabled -Dllvm=disabled`
  (glslang ≥12.2 was needed for turnip's BVH preamble — built 15.4.0 from
  source in the same chroot).

Deployment on the phone: `/opt/mesa-kgsl/lib` (libEGL, libGLESv1/2,
libgallium-25.3.0.so, libvulkan_freedreno.so) plus the turnip ICD json at
`/opt/mesa-kgsl/share/vulkan/icd.d/` with its library path rewritten.
Run env: `LD_LIBRARY_PATH=/opt/mesa-kgsl/lib
VK_ICD_FILENAMES=/opt/mesa-kgsl/share/vulkan/icd.d/freedreno_icd.aarch64.json
MESA_LOADER_DRIVER_OVERRIDE=zink EGL_PLATFORM=surfaceless`.

## THE RESULT (2026-10-24 00:30): the GPU writes memory. 4-for-4.

`tucomp.py` (pure-ctypes Vulkan compute, no EGL, no zink, no DRM — just the
system libvulkan loader + our Turnip/kgsl ICD):

```
device: Turnip Adreno (TM) 640  api 0x403148  type 1
compute queue family: 0
ok: vkCreateDevice ... ok: compute pipeline ... ok: queue submit
u32[0..7] = 0xa5a50000, 0xa5a50001, ... 0xa5a50007
pattern hits: 256/256
VERDICT: GPU WROTE MEMORY VIA TURNIP/KGSL COMPUTE
```

Re-run 3 more times in the same boot: **256/256 every time.**
`gpubusy` before 43 → after 227 (busy ticks, the GPU itself worked).
Zero SMMU faults, no oops, no BUG lines in dmesg.

The shader->DDR write path — the single oracle that NEVER passed in 287
images of the drm/msm driver — passes on the FIRST clean run of the KGSL
pivot, repeatedly.  Everything the project spent 287 images proving sound
(submission, MMU, zap, power) works when the same hardware is driven by the
driver Samsung/CAF shipped for it.

## GL via zink: WORKS (2026-10-24 01:20) — OpenGL ES 3.2 on the Adreno

The blocker was structural, and the fix is a two-part recipe:

1. **EGL must take the fd-less path.** `surfaceless_probe_device` opens
   `/dev/dri/renderD128` (the msm *display* card — the only DRM node on
   this kernel) and hands zink that DRM identity, which turnip/kgsl cannot
   match (no DRM node, no `VK_EXT_physical_device_drm`).  Setting
   `LIBGL_ALWAYS_SOFTWARE=1` + `MESA_LOADER_DRIVER_OVERRIDE=zink` routes EGL
   through `surfaceless_probe_device_sw` instead: fd=-1, driver "zink"
   (this is Mesa's own zink-fallback path, NOT software rendering).
2. **One 10-line patch to our zink build** (kept at
   `/home/usb/.opencode/zink_screen.c.patched-25.3`, section marked
   `PROJECT PATCH (d2q kgsl pivot)`): with `cpu` requested, no CPU-type
   Vulkan device, and exactly one non-CPU device, `choose_pdev` now takes
   that device instead of failing.  (Turnip reports the Adreno as
   INTEGRATED; upstream's CPU path wants `VK_PHYSICAL_DEVICE_TYPE_CPU`.)
   Also: EGL pbuffer attrs in 25.x are `EGL_WIDTH/EGL_HEIGHT`
   (0x3057/0x3056), not the legacy 0x3030/0x3031.

Result (`kgslgl.py`, surfaceless pbuffer, GLES 3.2 context):

```
renderer = zink Vulkan 1.3 (Turnip Adreno (TM) 640 (MESA_TURNIP))
version  = OpenGL ES 3.2 Mesa 25.3.0

T1 compute shader -> SSBO : 64/64 pattern hits — GPU WROTE BUFFER MEMORY
T3 fragment -> FBO + read : pixels (0,0,255,255) — GPU RENDERED BLUE
```

**Stability soak: 10/10 full T1+T3 passes** in one boot, gpubusy active
the whole time, **zero GPU faults** in dmesg.  Vulkan stress: 3x dispatch
of 1,048,576 words (4 MB) — **1M/1M pattern hits every time**.  A benign
segfault occurs in teardown AFTER `=== DONE ===` (ctypes exit path);
results are unaffected; noted for later hygiene.

Run env (the full recipe, all four lines required):

```
LD_LIBRARY_PATH=/opt/mesa-kgsl/lib
VK_ICD_FILENAMES=/opt/mesa-kgsl/share/vulkan/icd.d/freedreno_icd.aarch64.json
MESA_LOADER_DRIVER_OVERRIDE=zink
LIBGL_ALWAYS_SOFTWARE=1
```

(EGL_PLATFORM=surfaceless is implied by the harness's explicit
eglGetPlatformDisplay call.)

## Artifacts

* kernel: `/home/usb/mnt-sda/a6xx-build/out-gpu` (KGSL=y, drm display-only)
* image: `artifacts/flash/288-halium-boot-kgsl-debian.img` (sha256 above)
* Mesa build env (reusable): `/home/usb/mnt-sda/mesa-root` (Debian 12 arm64
  qemu-user chroot; mesa-25.3.0 sources; builds `build/` (gallium-kgsl,
  superseded) and `build-zink/` (turnip+zink, deployed))
* deployed stack on the phone: `/opt/mesa-kgsl/{lib,share}` + ICD json
* harnesses: `/home/usb/.opencode/tucomp.py` (+ `shader.comp.spv.b64`),
  `kgslgl.py`, `kgslinfo.py`, `run288.sh` — also on the phone in `/root`
* evidence: `evidence-288/results.txt` (kernel-side push), phone
  `/userdata/gpuwork/tucomp288.txt`

---

## ADDENDUM (2026-10-24 02:20) — PIXELS ON THE PANEL, then GPU-COMPOSITED WESTON

### 1. Direct GPU→panel rendering — works

The panel was already lit: DSI-1 enabled, `/dev/fb0` = fbcon's 1440×3040
XRGB32 fb (stride 5760), owned by a pixman (software) Weston that repainted
over everything. After `systemctl stop weston`, Weston's drm-backend restores
the fbcon fb as scanout — and direct fb0 writes become the screen content.

`kgslshow.py`: EGL surfaceless pbuffer (context only) → FBO 1440×3040 →
zink/Turnip/KGSL renders an animated plasma with channels pre-swizzled in
the shader (`.bgr` output) so glReadPixels lands in fb0's byte order — pure
row memcpys, no per-pixel CPU work: **2394 frames / 30 s = 79.8 fps at
native resolution**. Confirmed visible on the physical panel.

### 2. GPU-composited Weston — works (three fixes)

Stack: Weston 10 drm-backend on card0 + GBM (our Mesa build, `-Dgbm=enabled`)
+ EGL gl-renderer on zink → Turnip/kgsl, dma-buf shared buffers between
KGSL memory and the SDE/DPU scanout. Fixes needed, in order:

1. **GBM search path**: our libgbm looks in its chroot prefix
   `/usr/local/lib/aarch64-linux-gnu/{gbm,dri}` — created on the phone with
   symlinks (`dri_gbm.so`, and `zink/msm/kgsl/kmsro_dri.so` → our
   libgallium megadriver).
2. **zink DRM-identity** (choose_pdev part 2): with a GBM/card0 fd, zink
   matches pdevs by DRM identity; turnip/kgsl has none. Patch: identity
   match fails + exactly one non-CPU pdev → use it (part 1 already covered
   the fd-less path).
3. **Two kernel-age asserts**: Mesa 25 assumes `DMA_BUF_IOCTL_{IMPORT,EXPORT}_SYNC_FILE`
   (Linux 6.0+) and asserts on ENOTTY; this is a 4.14 kernel. Both patched
   to log once and continue without dma-buf implicit sync (worst case:
   tearing; none observed yet). Proper fix if tearing appears: backport the
   two ioctls to the 4.14 dma-buf layer.

Weston unit: `/etc/systemd/system/weston.service.d/gpu.conf` — drops
`--use-pixman`, sets `LD_LIBRARY_PATH=/opt/mesa-kgsl/lib`, the turnip ICD,
and `MESA_LOADER_DRIVER_OVERRIDE=zink`. Result: `systemctl is-active weston`
= active, desktop-shell up, gl-renderer chosen (`OpenGL ES 3.2 Mesa 25.3.0`),
GPU busy during compositing, power-collapses when idle. Known benign noise:
"computed repaint delay is insane" (vblank timestamp clock-domain quirk).

The turnip/kgsl dma-buf import/export path (`kgsl_bo_init_dmabuf`) is what
makes the whole zero-copy chain possible — display dumb buffers on card0
imported as GPU memory, rendered by the Adreno, scanned out by the DPU.

### Remaining polish (non-blocking)

- tearing check under load (if seen → backport the sync-file ioctls)
- the "insane repaint delay" clock quirk
- teardown-time segfault in the GL harness (cosmetic)

## ADDENDUM 2 (2026-10-24 02:40) — reboot survival + neofetch

* **GPU-Weston survives reboots**: the unit drop-in and /opt/mesa-kgsl are
  persistent, weston.service is enabled, and after a hard sysrq-b reboot the
  compositor came up on its own on the GL renderer (fresh log:
  `OpenGL ES 3.2 Mesa 25.3.0`, no --use-pixman in the live cmdline).
* **neofetch GPU line**: neofetch 7.1 detects GPUs via lspci (none on an
  SoC).  Override `get_gpu()` appended to `/root/.config/neofetch/config.conf`
  reading the live `/sys/class/kgsl/kgsl-3d0/gpu_model`:
  `GPU: Qualcomm Adreno 640v2 (Mesa Turnip/KGSL)`.
* **OPERATIONAL LESSON (cost one debugging cycle): `echo b >
  /proc/sysrq-trigger` is a HARD reset — files written shortly before it
  (still in delayed allocation) come back as NUL-padded tails after ext4
  journal replay.  The first neofetch edit was lost exactly this way (it
  "worked" from page cache pre-reboot, was never on disk).  **Run `sync`
  before sysrq-b reboots**, and treat "file is suddenly all NULs at the
  end" as the signature of this failure, not as a mystery restorer.

## ADDENDUM 3 (2026-10-24 12:00) — the five "cosmetic" items, all closed

1. **vulkaninfo now shows the Adreno** (`GPU0: Turnip Adreno (TM) 640`,
   INTEGRATED, llvmpipe demoted to GPU1).  Required: our ICD in
   `/usr/share/vulkan/icd.d/`, the dead broadcom/radeon ICDs removed (their
   enumeration failure poisoned the loader), and a turnip patch
   (`tu_knl_kgsl_load`): upstream hard-fails the WHOLE instance when
   VK_KHR_display is enabled — vulkaninfo enables it blindly — now degraded
   to warn-and-continue.
2. **Disk**: `/` was 100% full.  Cleanup (ElyPrismLauncher 391M +
   root-backup.tar 169M + old cap logs → /userdata, launcher symlinked;
   journald capped 50M) then **rootfs.img grown 2→6GB ONLINE** (dd append
   on /userdata + `losetup -c /dev/loop0` + `resize2fs`): `/` now 5.9G,
   4.6G free.  Note: run such ops DETACHED on the phone — ssh timeouts
   killed the first attempt mid-chain; and avoid bare `sync` (there are
   ancient D-state sync relics; a targeted `sync <file>` works fine).
3. **RNDIS IP dissociation fixed host-side**: NetworkManager profile
   `d2q-rndis` bound to the gadget's fixed MAC (22:c3:4e:73:85:3e) with
   static 10.15.19.100/24 — any re-enumeration (any interface name)
   auto-activates it.  Wildcard ifname matching did NOT work on this NM;
   MAC binding does.
4. **CPU "1.785GHz everywhere" is a measurement artifact**: policy0 (4×
   silver) tops at 1.7856GHz; policy4 (3× gold) 2.4192GHz; policy7 (prime)
   2.8416GHz — all reach full rated clocks under load (verified 2.84GHz on
   cpu7).  Tools that read only policy0 show the silver cluster's max.
5. **btop**: reads NVML/AMDGPU sysfs only — no Adreno support exists
   upstream; nothing to fix there.  The real "other programs" risk was
   discovery, now solved system-wide: our libEGL/libGLESv2/libGLESv1/libgbm
   replace the system 22.3 ones (originals backed up in
   `/opt/mesa-system-bak`), the turnip ICD sits in the system icd.d, and
   the Mesa stack was rebuilt with **x11+wayland platforms and GLX**
   (libGL.so.1.2.0 now shipped) — for X11/Wayland GL apps (Minecraft via
   GLFW/LWJGL, ElyPrismLauncher/PineconeMC).

Everything re-verified after deploy: Weston auto-restarts on the GL
renderer; vulkaninfo lists the Adreno first; GPU compositing active.

## ADDENDUM 4 (2026-10-24 14:30) — the Minecraft window-system campaign

Goal: a GL window protocol for GLFW/Minecraft on this stack. Findings in
order (all reproducible, all debugged to root cause):

1. **EGL-on-Wayland clients are architecturally blocked for zink/kgsl.**
   Mesa's zink client path is **kopper**, which requires Vulkan WSI
   (VK_KHR_wayland_surface); turnip/kgsl has none. With kopper disabled
   (`LIBGL_KOPPER_DISABLE=1`) the classic dri2 path lands on Mesa 25's new
   **dril** dri shim, whose context creation returns NULL by design — and
   whose fd==-1 helper recurses into libEGL passing EGL_DEFAULT_DISPLAY as
   a gbm_device* (strdup(NULL) SIGSEGV — patched in our build to return
   NULL; upstream assumes glvnd there). weston-simple-egl therefore cannot
   work; **Weston itself only works because it uses EGL-GBM, not EGL-Wayland.**
2. **Xwayland is a dead end for the same reason** (its glamor is an
   EGL-Wayland client) — explains the glxinfo hangs.
3. **Xorg + modesetting on card0 got the furthest ever**: after exiling the
   glvnd dispatchers + 22.3 vendor libs (ldconfig silently kept re-pointing
   libEGL.so.1 to glvnd — that was the "same crash" red herring), Xorg's
   glamor initialized **on the Adreno**:
   `glamor X acceleration enabled on zink Vulkan 1.3(Turnip Adreno (TM) 640 (MESA_TURNIP))`.
4. Xorg then SIGSEGVs in the modesetting DDX's RandR output-property loop
   (fault PC modesetting_drv.so+0xcf60: `ldr w0,[x0]` — NULL deref of an
   atom pointer in a 32-byte-entry property table; RRChangeOutputProperty
   observed called with valid args 12× before the crash). A
   `drmModeGetConnectorCurrent`→`drmModeGetConnector` LD_PRELOAD shim
   (/root/connshim.so) did NOT change it — the NULL is elsewhere in the
   CAF-sde connector property structures. **Next step: build xserver 21.1.7
   modesetting with debug info in the chroot and fix the property-table
   walk** (or patch the kernel's sde property exposure).

Phone state: Weston GPU desktop restored and active; / at 4.5G free;
xorg/openbox/gdb/xwayland installed (xorgdebs in /userdata; glvnd exiled to
/opt/mesa-system-bak/glvnd — restore with `ldconfig` after moving back).
Build env gains: libdrm-dev in chroot; connshim.c at /root.

## ADDENDUM 5 (2026-10-24 15:30) — 🎮 MINECRAFT-CLASS GLX ON THE PANEL

**The DDX crash is fixed with a 4-byte binary patch.** The debug-symbol
backtrace pinned it to `drmmode_output_set_property`
(drmmode_display.c:2965, xserver 21.1.7): `p->atoms[0]` with
`p->atoms == NULL` — the CAF sde exposes non-enum connector props, and
21.1.7 lacks the newer NULL guard. Patched the shipped
`modesetting_drv.so` at vaddr/file-offset 0xcf60: `ldr w0,[x0]`
(0xb9400000) → `cbz x0, loop-continue` (0xb4ffff20) — a NULL atoms entry
now simply doesn't match (original backed up as modesetting_drv.so.orig-2117;
patched copy also at /userdata/modesetting_drv-patched.so).

**Result — full classic Linux GL stack on the phone:**

```
Xorg :1 (modesetting+glamor on card0) — glamor X acceleration on the Adreno
glxinfo -B: direct rendering: Yes
             Device: zink Vulkan 1.3 (Turnip Adreno (TM) 640 (MESA_TURNIP))
             Accelerated: yes, Video memory: 8578MB
             Max core profile version: 4.6
openbox as WM; touchscreen = core pointer.
```

**PineconeMC (ElyPrismLauncher AppImage, --appimage-extract-and-run,
TMPDIR on /userdata) launched on :1 and runs** with the 1.21.1 instance
(the original LWJGL "no OpenGL context" cause is gone).

Boot persistence: `d2q-xorg.service` (enabled; weston disabled) — Xorg :1
+ openbox on boot with the full zink env. To return to Weston:
`systemctl disable --now d2q-xorg && systemctl enable --now weston`.

Remaining known-good-to-verify: Minecraft window on panel (user tapping
Launch), touch-as-mouse feel, and any tearing (sync-file ioctls still
absent in 4.14). Method chain that got here: dbgsym backtrace →
instruction-precise binary patch → no source rebuild needed.

## ADDENDUM 6 (2026-10-25 ~01:30) — the display war: won twice over, final wall named

Night-long campaign toward Minecraft-on-panel. Results in order:

1. **`fbcon=map:2` (image 290f) silences the console** — no more repaint war,
   and the fbdev commit keeps powering the panel at boot. Runtime
   `vtcon1` unbind WEDGES this kernel — never do it live (cmdline only).
2. **Xorg's red REACHED THE PANEL** (user-confirmed flash) — with fbdev=Y +
   D2QKMS mode2 + the DDX patch + mode-info fallback, Xorg+glamor+zink
   genuinely scans out. The render stack is fully proven on the physical
   display.
3. Kernel-side fix #4 shipped: `dsi_conn_get_mode_info` fallback
   (`dsi_drm.c`) — when a legacy SETCRTC hands a userspace-converted mode
   (private==NULL), borrow priv_info from the display's mode list
   (match h/v/pixel-clock; refresh may be 0 in umodes). This killed
   "failed to get mode info rc=-22" and made the initial modeset succeed.
4. Kernel-side fix #5 identified and SHIPPED: D2QKMS mode 2 — ghost
   encoders (Virtual/DP) banned from crtc-0 so userspace can never route
   DSI elsewhere ("failed to dispatch commit to any CRTC" eliminated).
5. **THE FINAL WALL (precisely named)**: any REAL mode switch (DMS) tears
   down the bootloader's DSI link; the fresh
   `dsi_pll_7nm_lock_status: PLL(0) lock failed, status=0x00000000` →
   `vco_7nm_prepare rc=-110` — **the DSI 7nm PLL never re-locks when
   kernel-enabled** (its power domain/rail presumably not brought up by
   sde_power_resource_enable). Every working path so far INHERITED the
   bootloader-configured PLL (continuous splash). Next session: dig
   `dsi_pll_7nm.c` vco_prepare (phy LDO/gdsc votes, ref clk, bias
   programming) vs what the bootloader left running.

Current images: 290f on the phone (fbdev=Y + fbcon=map:2 + d2q_dsi_crtc=2 +
all patches). Weston and Xorg(+openbox) both start; panel shows splash
content; console text is gone. GL stack unchanged and working.

## ADDENDUM 7 (2026-10-25) — the GDSC theory: disproven by two bootloops; the real lead is the RSCC

Images 291/292/293 tested "take GDSC ownership" (regulator_set_mode(NORMAL)
before PLL enable — rationale: gdsc_get_mode() reads HW_CONTROL_MASK and
msm_dss_enable_vreg() then skips regulator_enable; pre-mode logs CONFIRMED
the GDSC lived in FAST/HW-control mode from the bootloader).

Result: BOTH bootlooped, and the logs killed the theory:
* 291 (ungated): set_mode fired ~50x during the boot commit; bitclk
  set_rate -22 cascade; silent hang; XBL warm reset.
* 292 (gated on !handoff): STILL fired at boot — handoff_resources is FALSE
  during the boot commit (recalc never saw the PLL locked... or the atomic
  disable phase runs a vco unprepare first); pre-mode logs proved
  FAST(1)→NORMAL(2) transitions live.
* 293 (gated on a real-teardown flag): the boot atomic commit's disable
  phase ALWAYS runs a vco unprepare (needs_modeset teardown), arming the
  flag immediately — set_mode glitched the live domain again. 3rd bootloop.

**Conclusions:**
1. There is NO safe moment inside an atomic modeset for a GDSC mode flip:
   every commit runs disable-then-enable phases over a live domain.
2. The GDSC being HW-controlled is BY DESIGN on this platform:
   `af20000.qcom,sde_rscc` is a registered CONSUMER of mdss_core_gdsc —
   the RSC (Runtime State Controller) drives the GDSC in hardware as part
   of the display pipeline. Android never flips it to SW mode.
3. Therefore the DSI PLL lock failure (status=0x0 after a real DMS) is NOT
   a regulator problem. The remaining suspects, in order:
   a. **RSCC sequencing** — on a real mode switch the sde encoder/RSC
      must program the mode into the RSC and trigger the GDSC/interface
      handshake; our commit path may miss the RSC step entirely
      (see sde_rscc.c / sde_encoder_phys_cmd vs vid RSC handling).
   b. The DSI PHY lane/analog programming order in dsi_display_prepare
      vs the RSC handshake.
   c. What EXACTLY differs between the splash-inherited PLL (works) and
      a kernel-programmed one: dump PLL registers at lock-fail via the
      hang dump (mdss regdump at -110) instead of /dev/mem (MMIO reads
      hang when the AHB clock is gated).

Stable state restored: image 290f back on the phone (boot verified, Xorg
active, red set). 291-293 images retained in artifacts/flash for the record;
the gdsc patch is in the tree (d2q_need_own flag) but INERT — re-enable
only via a fresh flag-carrying build.

Recommended next session: read sde_rscc.c (af20000) + the cmd-mode encoder
phys (sde_encoder_phys_cmd.c) RSC trigger path, and compare with what the
splash handoff (ss_panel / cont_splash code in dsi_display.c) does; the
answer to "how does the RSC re-arm the GDSC on a real modeset" is the
missing piece. Also consider: force the cmd-mode→video-mode switch (POMS)
or simply stay on the splash PLL rate and only ever page-flip (fbcon-style
takeover, no modeset) — a compositor that never modesets avoids the PLL
entirely (Weston on 290f works this way!).

POST-ADDENDUM 7 (2026-10-25): the GDSC experiment (d2q_need_own flag,
D2QPLL set_mode block in mdss-pll-util.c, flag in mdss-pll.h and
mdss-dsi-pll-7nm.c) has been REVERTED from the sources entirely — the tree
is back to: D2QKMS (sde_kms) + dsi mode-info fallback (dsi_drm) + drm/msm
instrument guards + all Mesa-side patches. The phone runs 290f.

USER OBSERVATION on the current 290f boot: "no red, but panel is powered" —
on some boots Xorg's takeover lands (red flash), on others the splash stays.
That non-determinism fits the RSCC handover theory (the splash takeover
depends on commit timing vs the RSC state machine). The no-modeset route
(page-flip-only compositor, Weston-style) is the path that sidesteps the
PLL entirely.

---

## ADDENDUM 8 (2026-10-25 21:2x) — X/GL stack verified sound; the panel link is the whole problem

Boot `fac09159`, image 290f.  All checks done live over ssh.

**The X/GL stack is NOT the problem.**  `glxgears -info` on `:1` through the full
chain (GLX → glamor → zink → Turnip → KGSL) reports
`298-299 frames in 5.0 s = 59.5 fps`, continuously, with glamor X acceleration on
`zink Vulkan 1.3(Turnip Adreno (TM) 640)`.  `xrandr`: DSI-1 connected, 1440x3040.
Only benign `ZINK: DMA_BUF_IOCTL_{IMPORT,EXPORT}_SYNC_FILE not supported` noise.

**Minecraft on Wayland is impossible, not a bug** — the user's attempt was inside
Weston.  The one crash report on the phone (`crash-2026-10-24_12.03.57-client.txt`)
is `There is no OpenGL context current` with `GFLW Platform: wayland`: GLFW chose the
Wayland backend (inherited `WAYLAND_DISPLAY`), and zink/kgsl has no
`VK_KHR_wayland_surface` (Addendum 4).  On X11 with `WAYLAND_DISPLAY` unset that
crash cannot occur, and GLX is proven working above.

**The panel link dies after boot and does not come back.**  Kernel log:
```
dsi_phy_hw_v4_0_wait_for_lane_idle: lanes not in stop state, LANE_STATUS=0x00000000
dsi_phy_enable_ulps: lanes not entering idle, skip ULPS
dsi_phy_set_ulps: [DSI_PHY0] Ulps state change(1) failed, rc=-110
dsi_pre_clkoff_cb: failed enable ulps, rc = -22
```
repeating every 1–4 min (18 occurrences in ~20 min).  `LANE_STATUS=0` means the PHY
is not running at all when the driver tries to clock off.

**Idle collapse is NOT the trigger (falsified).**  With `glxgears` running flat out
for 60 s (continuous 60 fps commits): **zero** new ULPS failures (18 → 18), yet the
panel still showed *nothing* (user-confirmed).  So the link is already dead before
any idling; commits cannot revive it.

**Two structural oddities observed:**
1. `Virtual-1` (ghost connector) is attached to `crtc-1` and composes a 1024x768
   window of the *same* framebuffer; `fb0/modes` reports `U:1024x768p-0` while the
   buffer is 1440x3040.  Fix #4 only banned ghost encoders from crtc-0 — crtc-1 is
   still hosting one, on a separate CTL.
2. `allow_phy_power_off` is already `N`, so the PHY is not explicitly powered down;
   the failure is in the ULPS/lane-idle path off an already-dead PHY.

**Implication:** the remaining wall is exactly Addendum 7's: a real KMS modeset
(Xorg) leaves the DSI/PHY in a state where the link never re-establishes; Weston
apparently inherits the splash configuration instead of re-programming it.  The
no-modeset / inherit-the-splash route remains the only known-good display path.

---

## ADDENDUM 9 (2026-10-25 22:0x) — CORRECTION: the panel link was never dead. Two mundane bugs.

This supersedes the "DSI link is dead" reading of Addendum 8.  Controlled test:
`d2q-xorg` and `weston` disabled, replaced by a backlight-only oneshot
(`d2q-panel-backlight.service`, logs `PANEL-TEST` to /dev/kmsg), so nothing in
userspace touched KMS.

**Test result:** splash visible → black ~5 s after boot, with no X at all.  That
part is real and reproducible (see the 4.4–5.2 s window in
`evidence-panel-test/dmesg-backlight-only-boot.txt`):

```
4.467 msm_atomic_helper_commit_modeset_enables
4.468 dsi_display_enable: is_cont_splash_enabled
4.493 "samsung splash enabled.. skip image write"     <- splash handed over
4.499 brightness -> 0
4.512 dsi_display_pre_disable -> disable -> unprepare  <- torn down 50 ms later
4.706 modeset_enables AGAIN (no cont-splash) -> set_mode -> enable
4.961 DISPLAY_ON, brightness -> 2500
5.044 dsi_phy_enable_ulps: Failed to enter ULPS (request=0x1f, actual=0x1f)
5.044 dsi_pre_clkoff_cb: failed enable ulps, rc=-22 -> "pre link clk off cb failed"
```

So the boot commit does a **redundant disable→re-enable of the whole DSI** inside
one commit.  Worth fixing, but it is NOT what makes the screen black — see below.

### Bug 1 (the big one): the backlight is pinned at 6 nits

`panel0-backlight` reads **2500 of 50000** (≈6 cd/m²) after the boot commit
(`ss_brightness_dcs: level : 2500 candela : 6CD`), not the 20000 the DT/panel
sequence briefly sets.  At 6 nits in a lit room the panel is indistinguishable
from dead.  Writing **20000** makes it visibly lit and it *stays* (held 8 s,
then 10 s, then 28 s on three checks).  **This alone explains most of the
"panel is dark" reports.**

Ship: `d2q-backlight-assert.service` (enabled) — after `d2q-xorg` is up, waits
10 s, writes 20000 / bl_power 0, logs `PANEL-SET`.  Coexists with the X unit,
which also sets it.

### Bug 2 (the actual remaining blocker): X's framebuffer never reaches a plane

With X up and GLX proven working (glxgears 59.5 fps):

```
plane[60] crtc-0 (DSI-1):  fb=218  1440x3040 pitch 5760   <- fbcon buffer, all zero
plane[84] crtc-1 (Virtual-1): fb=218
X's framebuffer:             ID 30  1440x3040 pitch 5888   <- attached to NO plane
fb0 content: 00 00 00 00 everywhere (including the centre)
ulps_status = N  (link awake, not asleep)
```

X allocates and renders into fb 30, but the DSI pipe keeps scanning the fbcon
buffer (ID 218), which is black.  So the compositor's output never reaches the
CRTC — a modeset/page-flip problem at the X↔KMS boundary, NOT a DSI/PLL problem.
The earlier "red flash" is consistent with one transient successful commit,
after which the plane reverts to fbcon.

Note `Virtual-1` (ghost connector) still owns `crtc-1`; fix #4 only banned ghost
encoders from crtc-0.

**Next step:** instrument the atomic path (drm.debug=0x1e, or log the plane/fb in
`d2q_*_atomic_check`/`atomic_update`) and find why X's SetCrtc/flip does not
commit — suspect the D2Q encoder-routing patch (crtc-0 restriction) and/or the
fbcon restore re-claiming the primary plane.

### Method note
`LANE_STATUS=0` + repeated `Ulps ... failed, rc=-5/-110` are **not** proof of a
dead link: the PHY reports that same pattern while the panel is demonstrably
displaying content.  Do not use it as a death certificate; `ulps_status` and an
actual framebuffer read are the honest signals.

State left on the phone: `d2q-xorg` enabled+active, `weston` disabled,
`d2q-panel-backlight` disabled, `d2q-backlight-assert` enabled, backlight 20000.

---

## ADDENDUM 10 (2026-10-25 23:0x) — ROOT CAUSE of the black X screen: X re-applies a CACHED inherited state

Instrumented image **296** (`artifacts/flash/296-halium-boot-atominstr.img`,
sha256 6a8a9950…, kernel b4b39d55…): base 295 + `D2QATM` state dump in
`msm_atomic_commit`, `D2QVBL` + `dump_stack()` in `msm_disable_vblank`.
(kprobes/ftrace are NOT available in this kernel; `CONFIG_DRM_DEBUG_ATOMIC` is
compiled but CAF never dumps the atomic state, hence the custom print.)

### Verified sequence (image 296, boot #113)

```
[11.050] D2QATM[in] comm=Xorg  plane60 crtc=0 fb=208  → out: primary_fb=208   X's own fb SET, ret=0
[13.870] D2QATM[in] comm=Xorg  plane60 crtc=0 fb=218  → out: primary_fb=218   X puts fbcon's fb back
```
The revert commit contains the **entire inherited configuration**: crtc0 + ghost
crtc1 (1024x768), all 16 planes listed, plane-0 fb=218, plane-1 fb=218, the rest
crtc=-1/fb=0.  That is X re-applying its saved "desired modes", not a flip.

### The trigger is DAMAGE, and it is X's own doing

Bisected live:
* X start with **no client and no post-start actions** → plane stays on X's fb
  (`208`/`214`) for 12 s and more.  ✔ the initial modeset is fine.
* `xsetroot -solid red` (or blue/green) → immediate revert commit → plane 218.
* starting **openbox** (it paints the root) → same revert.
* running **glxgears** (window maps, draws) → same revert.
* `xrandr --output DSI-1 --auto` / explicit mode → does NOT recover; X sets 218.
* `/dev/fb0` stays all-zero after a blue root paint → X draws into its own fb,
  the scanout is the fbcon buffer.  Panel = black by construction.

fb identity (debugfs `/sys/kernel/debug/dri/0/fb`):
`fbcon fb 1440x3040 pitch 5760 ID 218` (refcount 5) vs `user fb 1440x3040
pitch 5888 ID 208/214` (X's).  So the revert swaps X's fb for the kernel's
fbdev framebuffer.

### Mechanism (xserver 21.1.7 modesetting)

The re-apply goes through `drmmode_set_mode_major()` with `allow_modeset=1`.
The fb it uses is `drmmode_crtc_get_fb_id()`, which falls back to
`drmModeGetCrtc()->buffer_id` when the driver has no fb id cached — i.e. it
can hand back the **fbcon fb that was on the CRTC at X startup**.  So X's own
fb is correct, but every mode re-apply uses the stale/inherited fbcon fb.

### Fix options, in order of preference

1. **Kernel (best): make the fbdev framebuffer unavailable to masters.**
   When a DRM master performs its own modeset, suspend/release the fbdev
   (`drm_fb_helper_set_suspend_unlocked` / drop the fbcon fb) so the inherited
   path has nothing to fall back to.  Also consider `msm_drm.fbdev=0` as a
   one-shot experiment — if X then works, fbdev inheritance is proven as the
   sole cause (costs one cmdline change; risk: panel power-on at boot, which
   fbdev=Y currently provides).
2. **xserver (targeted patch):** after any successful modeset, update
   `drmmode_crtc->fb_id`/`drmmode->fb_id`; and/or never accept the fbdev fb
   (pitch 5760 / `FB_ID` belonging to the console) as a modeset target.
   Needs the 21.1.7 source (not on disk; build env exists at
   `/home/usb/mnt-sda/mesa-root`).
3. **Workaround:** avoid anything that triggers a re-apply — but openbox and
   every GL client trigger it, so this is not viable.

State left: d2q-xorg restored to its original unit (xsetroot+openbox), X
running, backlight 20000, instrumentation still in the running kernel (logs
only, capped at 160 dumps).

---

## ADDENDUM 11 (2026-10-25 22:2x) — FIXED: image 297, fbdev guard. The panel shows X.

Image **297** (`artifacts/flash/297-halium-boot-fbdevguard.img`,
sha256 a7e9cdea…, kernel 21e8273f…): 296 + `d2q_guard_fbdev_fb()` in
`msm_atomic_check()` (msm_drv.c).  A *userspace* atomic commit that assigns the
fbdev framebuffer to a plane whose current fb is a different (client) fb is
refused with -EINVAL.  Kernel-context commits (splash handoff, fbcon restore)
are untouched.

### Verified in silicon (boot #114)

```
D2QGUARD: refusing fbcon fb 218 on 'plane-0' (crtc0 had fb 30, comm=Xorg/922)
D2QGUARD: refusing fbcon fb 218 on 'plane-0' (crtc0 had fb 219, comm=Xorg/1241)

plane[60] (DSI-1): fb=219     <- X's user fb
fbcon fb 218: refcount 5 -> 1 (no longer on any CRTC)
```

**User-confirmed: the panel shows the RED X desktop** (first non-black frame in
this whole campaign).  Remaining cosmetic: a few small black patches at the top
= unfilled regions of X's freshly created fb; expected to disappear once
something paints there (verify with a full-screen client).

### Why it worked
X's own modeset was always fine; xserver 21.1.7 re-applies a cached inherited
state on every damage event and that state carries the fbcon fb.  Rejecting
exactly that transition keeps X's fb on screen, with no X patch needed.

### Minecraft status (NOT a graphics problem any more)
`PineconeMC --launch 1.21.1` now starts but aborts during component resolution:

```
[launcher.instance.profile.resolve] Remote task 0 failed: "Failed to finalize validators"
Component list update/resolve task failed. Reason: "Could not download metadata for Minecraft 1.21.1"
```

Network itself is healthy (piston-meta.mojang.com 200, pineconemc.ru 200);
`meta.prismlauncher.org/v1/<uid>.json` returns 404, and the fork's own meta host
times out.  So the launcher cannot refresh metadata; the instance itself is
fully installed (`libraries/`, `assets/`, `meta/net.minecraft`).

Next: either (a) fix the launcher's meta source / make it launch from cached
metadata, or (b) launch Minecraft directly from the resolved version JSON,
bypassing the launcher.  The display, GLX, GL and GPU paths are all proven
working now.

## ADDENDUM 12 (2026-10-25 22:3x) — X desktop: LXDE on the guarded Xorg

Installed (Debian bookworm, apt): `lxde-core lxpanel pcmanfm lxterminal feh
x11-xserver-utils`.  apt was initially blocked by a half-finished transaction
(5 packages left `iU`, incl. `xserver-xorg-core` and its dbgsym pin to u12 while
u13 was installed) — `dpkg --configure -a` + dropping the stale dbgsym fixed it.

Desktop stack in `d2q-xorg.service.d/de.conf` (persistent across reboots):
openbox (WM) + lxpanel (panel) + pcmanfm --desktop (icons + wallpaper).
Wallpaper `/usr/share/backgrounds/d2q.png` (generated gradient, pcmanfm config
`/root/.config/pcmanfm/LXDE/desktop-items-0.conf`).
Launchers: `/usr/local/bin/pineconemc` (sets the zink/kgsl env, unsets
WAYLAND_DISPLAY, runs the AppImage with --show-window), desktop icons
`PineconeMC` and `Terminal`, plus `/usr/share/applications/pineconemc.desktop`.

Verified: openbox+lxpanel+pcmanfm running, plane scans X's own fb (217), the
fbdev guard keeps rejecting the console fb (comm=Xorg), backlight 20000.

Remaining (not graphics): the launcher cannot refresh component metadata
(`meta.prismlauncher.org` 404 / fork host timeout) so `--launch` aborts before
spawning Java.  The instance and libraries are installed; launching from the
cached version JSON or fixing the meta source is the remaining step.

---

Full change log for the 2026-10-25/26 session: `CHANGES-2026-10-25-26.md`
(images 296/297/298, Turnip assert fix, systemd units, LXDE, network/clock).

---

## ADDENDUM 13 (2026-10-26) — the 1 s desktop lag root-caused: msm_fbdev_dirty; image 299 built

### Symptom
X11 desktop usable but laggy "as dog's shi": selection box trails ~1 s behind the
touch while the cursor moves normally. Minecraft 1.21.1 (GL/zink): 20 fps with
frametime spikes. GPU idle during the lag (gpubusy ~1.3 %) → not GPU-bound.

### Evidence chain (all on boot #117, kernel #115 = image 298)
1. `dmesg`: D2QGUARD fires at a dead-steady **1.5 s cadence** (substituting fb 30
   for fbcon fb 218 on plane-0, comm=Xorg/1447) — 492 times in 23 min.
2. `strace -f -p <Xorg>`: the ONLY periodic ioctl is
   `ioctl(14, DRM_IOCTL_MODE_DIRTYFB) = 0` every 1.5 s (main thread), preceded by
   a KGSL submit from the glamor thread — i.e. a client damage flush (some widget
   redraws every 1.5 s).
3. No SetCrtc/PageFlip/AtomicCommit ever issued by Xorg (gdb breakpoints, 45 s)
   — the modeset-class commits seen in dmesg happen INSIDE the kernel during the
   dirtyfb ioctl. Addendum 10's "X re-applies cached state" theory was WRONG:
   `comm=Xorg` in D2QATM is just the syscall context of our own kernel hook.
4. Root cause: `msm_fb.c:86` wires `.dirty = msm_fbdev_dirty` onto EVERY msm
   framebuffer (a d2q-era patch meant to kick the cmd-mode panel). Its body calls
   `drm_fb_helper_restore_fbdev_mode_unlocked()` — a full modeset-class commit
   that re-applies the FBCON configuration (fbdev fb on primary, ghost crtc-1
   re-asserted) from inside X's dirtyfb syscall. Under X that is EVERY damage
   flush → each repaint pays the whole sde encoder/DSI commit path (~1 s); the
   297/298 fbdev guard existed only to fight these commits' fb assignments.

### Fix — image 299 (`artifacts/flash/299-halium-boot-dirtykick.img`,
sha256 `cec2805166a8eb3f97756fa52dbd86598a1cbea50205b41c12229d289acfbfcd`,
kernel built 2026-10-26 18:28, cmdline+ramdisk byte-identical to 298, cmdline
454 B):
`msm_fbdev_dirty` rewritten: a DIRTYFB on an fb that IS on a plane now runs a
**plane-only atomic commit re-asserting the plane's CURRENT state** (kick — the
DPU refreshes, same mechanism as hardware-cursor updates, no modeset, no fbcon
restore, logs `D2QDIRTY: kick fb N planes=k ret=r` capped at 20 lines). An fb
on NO plane falls back to the old fbcon restore. The 298 fbdev guard stays
(defense in depth). Screen content flows via glamor into fb 30 regardless; the
kick only triggers the panel refresh, which is REQUIRED on this cmd-mode AMOLED
(the panel only shows a new frame on a commit — deleting the hook outright
would freeze the desktop between cursor moves).
Packer notes: `pack-d2q.sh` RAMDISK_SHA is env-overridable (default pin
unchanged). FIRST PACK OF 299 WAS BROKEN BY ME: bypassing the REFERENCE
header-diff check and slicing the ramdisk out of 298 past EOF (the file is
TRUNCATED — images 290-298 all carry only 32 KB of the 4,117,936-byte ramdisk
in-file and booted off the tail the 289 flash left on the BOOT partition;
canonical full ramdisk sha eb5d2eb5…, recovered from 288). ALWAYS run the
REFERENCE header-diff check (differences must be id-field only) — same ksize
48,536,898 for 298/299, so the check applies. Corrected pack: sha
4b335e2a…, ~52.8 MB, first self-consistent image since 289.
FLASHED 2026-10-26 ~19:10, VERIFIED: D2QGUARD 0 (was 492/23 min), D2QDIRTY
kicks ret=0 from t=8 s, DIRTYFB ioctl duration 8-13 ms (strace -T) vs ~1 s
before — Xorg main thread no longer blocks on the sde commit path.

### MC 26.3 native Vulkan: the docs' "no WSI" conclusion is STALE; real cause = GPU fault
* The build-zink Mesa config has had `platforms=['x11','wayland']` since 10-24;
  the deployed libvulkan_freedreno.so (sha f45b79be…) contains
  VK_KHR_{xcb,xlib,wayland}_surface. Surface creation in MC works.
* `vkcube` (vulkan-tools installed) on :1: selects the Adreno, runs 14 s, no
  error, no GPU fault → the WSI present machinery (kgsl dmabuf export is just
  `dup(bo->shared_fd)`; prime/DRI3 path) is functional.
* Live repro (headless `--launch 26.3`, stderr captured): **kgsl iommu fault**
  — `FAULTING BLOCK: UCHE: SP | VSC | VPC | PC | LRZ`, `fault_addr 0x0`,
  `svm_addr_mapping_check … mapped: 0`, then `gpu fault ctx 3 ts N status
  0080000[45] ib1 0x51105A000 ib2 0x5048F4DCC` — identical across runs.
  Turnip marks the device lost (kgsl_device_check_status / submit path) and MC
  dies at the next present with `VK_ERROR_DEVICE_LOST: Failed to present image`.
  Present is the messenger, not the cause.
* Env flags do NOT dodge it: `TU_DEBUG=sysmem` and `TU_DEBUG=nolrz` both fault
  at the SAME ib2 → GMEM resolve and LRZ ruled out. Deterministic
  turnip/renderpearl bug on the a640 (MC 26.3 "renderpearl" Vulkan frontend).
* Next levers for that dig (separate session): `TU_DEBUG=rd` cmdstream capture
  at the faulting IB, minimal-Vulkan repro (multi_draw? shader_draw_parameters?
  renderpearl feature list: wireframeFillMode, shaderDrawParameters,
  multiDrawDirectInterleaved, persistentMapping), and web search for other
  Adreno reports against 26.3.

### Misc this session
* `lightdm` (installed with lxde-core, was starting a stray Xorg :0 greeter —
  no master, ~0.5 % CPU) → `systemctl disable lightdm`. Session apps confirmed
  on DISPLAY=:1 (d2q-xorg). Stopped for real at next reboot.
* vulkan-tools installed on the phone (no mesa deps).
* Phone-side facts: `five-diag` logs every exec'd binary (floods dmesg);
  D2QATM[in] dumps are capped (only [out] lines after the first 160); udevadm
  monitor shows NO uevents on card0 (hotplug was never the trigger).
