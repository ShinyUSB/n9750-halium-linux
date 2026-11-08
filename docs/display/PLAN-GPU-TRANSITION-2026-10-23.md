# PLAN — transition from image 287 to a fully functional GPU

Date: 2026-10-23
Read after: `STATE-AFTER-SESSION-2026-10-21.md` (which covers images 276–287),
`PLAN-GPU-ZERO-TO-FULL.md` (the 2026-10-15 plan whose §7 this file supersedes
in detail), `consult/PLAN.md` (X1–X6).

---

## 0. Executive summary

The 287-image campaign proved the submission path sound and localized the
defect to the 3D domain under the 4.14 CAF **drm/msm** driver. Two facts now
dominate the strategy:

1. **The bug we are hunting lives only in the CAF drm/msm a640 port.** The
   same phone, same kernel family (ShinyKernel 4.14.190), same TZ/zap/GMU
   firmware renders fine under Android with the in-tree **KGSL** driver
   (`/sys/class/kgsl` ground truth, Oct 22). And the same SoC+GPU (SM8150 +
   Adreno 640 + GMU) renders fine under **mainline** drm/msm + freedreno on
   the Galaxy Tab S6 (bentlybro, Aug 2026). The silicon, the firmware, the
   power sequence and the memory path are all exonerated by other people's
   working stacks.
2. **The two "CX domain unpowered" leads from Oct 22 are weaker than the
   state doc says** (see §1) — one is an apples-to-oranges dump artifact, the
   other is a register-decoding error.

Therefore the plan is two tracks, run in parallel:

* **Track B — "KGSL pivot" (days): make the GPU *functional* now.** Boot the
  existing Debian with the known-working in-tree KGSL driver bound to
  `2c00000.qcom,kgsl-3d0`, and run a from-source Mesa (≥ 24) with
  `-Dfreedreno-kmds=kgsl` (freedreno GL/GLES natively over KGSL; Turnip
  Vulkan + Zink as fallback). This reuses 100 % of the existing boot
  infrastructure and drops the drm/msm-specific PM dance.
* **Track C — "mainline pivot" (1–3 weeks): the robust endgame.** Project
  Aloha UEFI (three sibling Samsung SM8150 targets already supported:
  `samsung-beyond1qlte`, `samsung-winner`, `samsung-gts6lwifi`; d2q is a
  mechanical copy) + sm8150-mainline kernel + the existing Debian 12 rootfs
  with kmsro/freedreno. Proven end-to-end on gts6l with published patches.
* **Track A — closure experiments (timeboxed, optional):** the two cheap,
  decisive measurements the Oct-22 session already identified (idle-vs-idle
  ground truth; aperture-corrected cluster readback). Do them for the record
  and for the remote chance they name the CAF bug — but they no longer gate
  anything.

Definition of "fully functional GPU" (unchanged from PLAN-GPU-ZERO-TO-FULL §6):
real GL workload on FD640 with sound readback; survives cold boot; visible
output through *some* scanout path (firmware framebuffer + kmsro is
acceptable).

---

## 1. Corrections to the Oct-22 conclusions (why "CX domain unpowered" is downgraded)

### 1.1 The `deafbead` 0x8e00 block is very likely a dump-method artifact

The working-side values came from the **KGSL snapshot**
(`/sys/class/kgsl/kgsl-3d0/snapshot/dump`). CAF's
`adreno_a6xx_snapshot.c:996-1001` **programs `CP_APERTURE_CNTL_HOST` per
cluster before reading cluster registers**:

```c
aperture_cntl = ((cluster->id & 0x7) << 8) | (ctxt << 4) | ctxt;
kgsl_regwrite(device, A6XX_CP_APERTURE_CNTL_HOST, aperture_cntl);
```

The 0x8exx registers are PS-cluster **context-banked** registers (RAC/RBP
sub-blocks, selected via `RB_RB_SUB_BLOCK_SEL_CNTL_HOST`). Our Debian dump
(`a6xx_cp_state_regs`, image 283+) reads them **raw, with the aperture never
programmed** — so `deafbead` is the expected poison for "no aperture
selected", not evidence of an unpowered block. Consistent details:

* `CP_APERTURE_CNTL_HOST` reads **0** on the working device *at rest* too
  (ground-truth dump) — the real values at 0x8e01/0x8e0c/0x8e3b were read
  mid-snapshot with the aperture switched.
* `0x8e50` (RB_CONTEXT_SWITCH_GMEM_SAVE_RESTORE) **does** read on ours
  (0x0) — it sits outside the banked sub-block select, and it differs
  0-vs-1 plausibly because the working GPU was *busy rendering* (state doc's
  own caveat).
* Image 267 already learned the hard way that touching the aperture against
  a **live** CP wedges the GPU (`ANALYSIS-FOUR-CANDIDATES-2026-10-18.md` §on
  the freeze) — the aperture is a global bus-mux for host reads *and* CP
  decode. The safe variant is in §4.2.

### 1.2 The `GPU_CC_CX_GDSCR` "bit 23" decode is arithmetically wrong

`ours = 0xf8222800`, `working = 0xf8228000`. XOR = `0x0000A800` → **bits 15,
13, 11** — not bit 23. In the standard GDSCR field layout (see
`drivers/clk/qcom/gdsc-regmap.h` in this tree) bits [15:12] are
`CLK_DIS_WAIT` (ours 2, working 8), and bit 11 is in the retention-adjacent
field. Crucially **both values have `PWR_ON` (bit 31) set** — neither domain
is collapsed. So the register shows a *timing/retain-config* difference, not
"the CX domain is off". On top of that, KGSL's own `a6xx_gmu_cx_is_on()`
treats this register as **unreliable on this part** and answers from
`regulator_is_enabled()` instead (`STEPS-1-2-3-CXGDSC-ANALYSIS.md`).

Net: the "CX domain not powered" theory has no leg left that survives
scrutiny. It should not drive the next 5 images.

### 1.3 What remains genuinely unexplained in the CAF driver

Everything in `STATE-AFTER-SESSION-2026-10-21.md`'s closed-negatives table
still stands; the remaining raw diffs (after 285/287) are the 0x54x VFD-ish
block, `CP_SQE_CNTL` 1-vs-0, `GBIF_SCACHE_CNTL1` 0-vs-`0x00c6318c`, and the
busy-vs-idle confound. None has a mechanism that survives §1.1/§1.2. After
287 images at roughly one hypothesis per flash, the expected value of the
next register guess is low — the state doc's own recommendation ("stop
eliminating one register per flash") applies.

---

## 2. Track B — the KGSL pivot (fastest path to a functional GPU)

**Idea:** stop fighting the drm/msm fork; boot Debian on the driver that
already works on this exact device. KGSL is in this very tree
(`Kernel/drivers/gpu/msm/`, `CONFIG_QCOM_KGSL=y` in
`d2q_chn_hk_defconfig:5655`), binds natively to the stock DT node
(`sm8150-gpu.dtsi:85`, compatible `qcom,kgsl-3d0` — the node our drm driver
borrows via the backwards-compat match at `adreno_device.c:413`), and its
full boot (GMU, HFI, zap, SMMU) is proven by the Android reference boot.

Mesa's KGSL kernel-mode-driver support (`-Dfreedreno-kmds=kgsl`) serves
**freedreno Gallium GL/GLES** and **Turnip Vulkan**; it is what Android
GSI/Termux community builds use. Debian 12's stock Mesa 22.3 predates it →
build Mesa ≥ 24 from source (cross-build in the existing `kernel-build`
distrobox; `TMPDIR` on mnt-sda per `BUILD-ENV-DISK-AND-CONTAINER.md`).

### B.1 Steps

1. **Image 288 "kgsl-debian"** — same tree, one config change:
   `CONFIG_DRM_MSM=n` (the two drivers both match `qcom,kgsl-3d0`; the
   cleanest way to hand the node to KGSL is to not build drm/msm at all —
   keep `CONFIG_QCOM_KGSL=y`, `CONFIG_QCOM_KGSL_IOMMU=y`). Everything else
   byte-identical to 287's cmdline (drop the `a6xx.*` experiment params —
   they are drm/msm module params and become inert).
   *Display note:* drm/msm owns the MDSS path; image 288 is a **headless
   GPU image** (the normal operating mode of this project anyway — RNDIS
   SSH). Display work stays on image 287. This is acceptable: the goal of
   288 is GL/Vulkan output into buffers, verified by readback.
2. **Firmware:** KGSL loads `a640_gmu.bin` and `a640_zap.mdt` through the
   same `request_firmware()` path — both were restored to `/lib/firmware`
   on Oct 21 (`STATE-AFTER-SESSION`, "Environment repairs"). Nothing to do,
   but verify with dmesg on first boot (`pil-a640_zap active_count(1)`).
3. **Mesa cross-build** (host, in the container):
   `meson setup build-aarch64 -Dgallium-drivers=freedreno -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl -Dplatforms=surfaceless -Degl=true -Dgles1=enabled -Dgles2=enabled -Dglx=disabled`
   (+ `zink` for the GL-on-Vulkan fallback). Reference configs:
   `DvaMishkiLapa/mesa-turnip-kgsl-build`, Termux's mesa config
   (`freedreno-kmds: msm,kgsl`). Deploy to the phone under `/opt/mesa-kgsl`
   with `LD_LIBRARY_PATH` — do **not** overwrite the system Mesa 22.3 yet
   (the drm/msm path stays available on image 287).
4. **Smoke test** (existing harness conventions, `wrtest.py`-style ctypes
   EGL): surfaceless pbuffer `glClear` + `glMapBufferRange` readback, guard
   on `GL_RENDERER` containing `FD640`, health via dmesg/kgsl nodes
   (`gpuclk`, `gpubusy` under `/sys/class/kgsl/kgsl-3d0/`). Then the T1
   compute→SSBO and T3 fragment→FBO oracles from `run287.sh` — these are the
   two workloads that never wrote anything in 287 images.
5. **If GL works:** run `glmark2-es2 --off-screen` (or equivalent) for a
   stability soak; then decide whether to keep both stacks
   (kgsl image for compute/GL, 287 for display experiments) or drop the
   drm/msm line entirely.
6. **If freedreno-GL-over-kgsl misbehaves** (least-likely branch): fall back
   to Turnip+Zink — install `vulkan-tools`, run the equivalent compute
   write via Turnip (KGSL backend is Turnip's most-exercised path), Zink on
   top for GL. This isolates whether the gap is in fd6-over-kgsl glue.

### B.2 What Track B deliberately does not solve

* On-screen output (KMS/compositor) — separate work item; headless first.
  (Options later: run weston with the RDP backend or a wlroots headless
  backend with hardware GL; or wait for Track C where kmsro+simpledrm is the
  proven display route.)
* Suspend/resume polish, cold-boot ergonomics (KGSL is actually *simpler*
  here: no `a6xx_gpu_load` dance, no 300 s rule, no PM pinning — those were
  drm/msm-bringup artifacts).

### B.3 Risks

| risk | likelihood | mitigation |
|---|---|---|
| KGSL bind fails on the Debian boot (DT/regulator differences vs Android boot) | low — same kernel, same DTB | dmesg compare vs Android boot; the Android reference boot used a sibling build of this same tree |
| fd6-over-kgsl gaps in Mesa for a640 | medium-low | Turnip+Zink fallback (B.1 step 6) |
| no /dev/dri at all breaks some harnesses | certain, by design | the kgsl backend needs no DRM fd; harnesses use EGL surfaceless, not raw DRM ioctls |
| cross-build yak-shaving (glslang etc.) | medium | container already has the toolchain; budget half a day |

---

## 3. Track C — the mainline pivot (robust endgame)

**Idea:** the PLAN-GPU-ZERO-TO-FULL §7 "Phase 4 alternative", now with a
*proven, published, Aug-2026* worked example on the same SoC+GPU+vendor
(Samsung): [bentlybro.com — Mainline Linux on the Galaxy Tab S6](https://bentlybro.com/posts/mainline-linux-on-the-galaxy-tab-s6-a-32-byte-field-and-one-line-of-device-tree)
(repo: `Bentlybro/gts6l-mainline-linux`, kernel `sm8150-linux-mainline/linux`).
Their GPU bring-up "took an afternoon": a640+GMU on mainline needs only the
dtsi node, the zap shader (already in our `/lib/firmware`), the stock
`a630_sqe.fw`, and a handful of small fixes (headless DRM master, missing
speed-bin → bin 0 via `opp-supported-hw`, `msm_use_mmu()` on a headless
master, 64-bit DMA mask on simpledrm — several already upstream by 6.18).

Project Aloha (`Project-Aloha/mu_aloha_platforms`) today lists three Samsung
SM8150 targets with DSDT support: **beyond1qlte (S10), winner (Fold),
gts6lwifi (Tab S6 WiFi)**. d2q is not listed — it is a mechanical sibling
port (same ABL family, same SoC; Note10+ is closest to beyond1/winner).

### C.1 Steps

1. **Aloha target `samsung-d2q`** (host-only work, phone untouched):
   copy `samsung-beyond1qlte` (or `winner`) target; adapt the DSDT for
   Note10+ hardware (the 4.14 Samsung DTS in this tree is the GPIO/
   regulator/panel source of truth); Binaries from d2q's own firmware.
2. **Boot chain:** `fastboot boot` the Aloha image first (flash nothing);
   bentlybro's route — magiskboot-repack to keep the Samsung ABL trailer —
   is documented for Samsung SM8150 and avoids the BCB pitfalls. d2q ABL
   is the same generation as Tab S6's.
3. **`linux-d2q.dtb`:** start from `sm8150-mainline`'s gts6l/beyond1 DTS;
   port panel/touch/regulators from the 4.14 tree; `clk_ignore_unused
   pd_ignore_unused` (proven requirement on this platform); keep the ABL
   framebuffer untouched for simpledrm.
4. **First-light milestones:** (a) UEFI shell on screen / RNDIS-serial
   console; (b) mainline kernel to userspace over the existing Debian 12
   rootfs (keep the rootfs; only boot chain + kernel change); (c)
   `renderD128` appears with FD640; (d) the existing ctypes harness
   (system Mesa 22.3 works — kmsro is included) produces the first real
   pixels/compute writes; (e) kmsro+simpledrm scanout + weston/KWin.
5. **Then** retire the Halium kernel for daily use; keep it bootable via
   the existing flash images for comparison (it remains the only way to run
   the Android reference for ground-truth dumps).

### C.2 Risks

| risk | likelihood | note |
|---|---|---|
| d2q ABL rejects the Aloha payload (per-device quirk) | medium | gts6l/beyond1 are the same ABL generation; magiskboot-repack route documented |
| device-specific DTS churn (panel, PMIC minor revs) | certain | mechanical; the 4.14 DTS in-tree has all the raw data |
| modem/wifi/etc. remain unsupported on mainline | known | not GPU scope; USB + display are what this track needs, both proven on SM8150 |
| scope creep into a full daily-driver port | certain ;-) | timebox to "GPU + display + USB + rootfs on UFS" and stop there |

---

## 4. Track A — closure experiments (timeboxed, cheap, for the record)

Only two, both already designed by earlier sessions. They no longer gate
Tracks B/C; run them in the gaps (each is one Android reboot, no kernel
build) mainly so the CAF story closes with evidence rather than abandonment.

### 4.1 Idle-vs-idle ground truth (the Oct-22 "method gap")

Boot the Android reference (existing `android-boot-live.img` flow), screen
off, wait for `gpubusy` → 0, take `/sys/class/kgsl/kgsl-3d0/snapshot/dump`,
re-diff against `debian-regs-287.txt` with `diff_regs.py`. Decision rule
(already written in the state doc): if `CX_GDSCR` / `0x8e50` / 0x8exx
differences persist at idle → real config gap; if they vanish → the whole
"CX domain" branch was an activity artifact. Zero build cost, ~1 h.

### 4.2 Aperture-corrected cluster readback (consult X1, made safe)

Do it **only with the CP quiesced** — from the existing `a6xx_gpu_cpstate`
dump path with the ring idle (RB_WPTR==RB_RPTR, no submits in flight), or
one-time at the end of `hw_init` before the ring starts. Sequence per KGSL:
for cluster {GRAS=3, PS=5} × ctxt {0,1}: write
`CP_APERTURE_CNTL_HOST = (cluster<<8)|(ctxt<<4)|ctxt`, for PS also
sub-block sel 0/9, read `GRAS_2D_DST_BR` / `RB_MRT_BASE_*`, restore
aperture 0. Expected outcome given §1.1: the 0x8exx block comes alive and
the deafbead "difference" dissolves; whether Mesa's state is visible in
silicon (X1's real question) is then answerable for the first time. One
build, one flash, low risk if — and only if — the quiesce precondition is
enforced in code (image 267's freeze is the counterexample).

### 4.3 Explicitly not planned

More single-register hypotheses (GBIF re-test, `CP_SQE_CNTL`, 0x54x block,
`GBIF_SCACHE_CNTL1`) — each is one build+flash against an oracle with a
poor hit rate, and Track B makes them moot within days. If Track B lands,
the CAF drm/msm driver is retired as a *component*, and its remaining
mysteries become archaeology, optionally resumed later with the mainline
driver as a register-by-register reference (which is much easier to diff
than a busy Android snapshot).

---

## 5. Sequencing and decision gates

```
Week 0 (now)
 ├─ B.1 image 288 (config flip)  ── build/flash same day
 ├─ B.3 Mesa cross-build          ── in parallel on the host
 ├─ A.4.1 idle ground-truth       ── in a gap (Android reboot, no build)
 └─ C.1 Aloha samsung-d2q target  ── host-only, start in the background

Gate G1 (image 288 + Mesa smoke): FD640 over /dev/kgsl-3d0 + T1/T3 write?
 ├─ YES → GPU declared functional (headless). B.5 soak; display decision:
 │         either wlroots-RDP/headless compositor on 4.14, or accelerate
 │         Track C which solves display natively. Continue C as the
 │         endgame; A optional archaeology.
 ├─ fd6-kgsl glue broken → B.1 step 6 (Turnip+Zink). Vulkan writes?
 │    YES → functional via Zink; GL-native gaps become Mesa bug reports.
 │    NO  → the first real surprise: KGSL itself fails on the Debian boot
 │          (vs Android). Diff dmesg vs the Android boot of the same tree;
 │          this would be a *userspace-environment* difference (init,
 │          pd_ignore_unused, mount ordering), not a driver one.
 └─ Gate G1 blocks > 1 week → promote C to primary; B is retried only
    after C's first mainline GPU light (the Mesa kgsl build is reusable
    there too if the 4.14 route is ever revisited).

Gate G2 (C first light): renderD128 + FD640 pixels on mainline?
 ├─ YES → migrate daily use to mainline; Halium kernel kept as reference
 │         boot for ground-truth dumps (A.4.1 becomes the standard diff
 │         method against a known-good *mainline* idle dump too).
 └─ d2q ABL blocks UEFI after honest effort → bentlybro magiskboot-repack
    variant, else document and stay on B; revisit when Aloha grows a d2q
    target.
```

Recommended priority if time is scarce: **B (all of it) → C.1 → A.4.1 →
C.2-C.5**, with C proceeding opportunistically in host-only time (no device
needed until the first `fastboot boot`).

---

## 6. Environment notes (unchanged constraints, for whoever executes)

* Build via `bash /home/usb/.opencode-tmp/build-gpu.sh` from the **host**
  (enters `kernel-build` distrobox); keep `TMPDIR=/home/usb/mnt-sda/tmpbuild`
  (`/home` is 100 %-full-adjacent; see `BUILD-ENV-DISK-AND-CONTAINER.md`).
* New images/artifacts go to `/home/usb/mnt-sda` (working drive) and
  `artifacts/flash/` per convention; nothing may be deleted from
  `artifacts/`.
* Device ops: `./u.sh --fix` after every reset; reboot via
  `echo b > /proc/sysrq-trigger`; the drm/msm-specific rules (300 s wait,
  `a6xx_gpu_load`, PM pinning, `a6xx_gpu_cpstate` health checks) apply only
  to Track A images — KGSL (B) and mainline (C) have their own, ordinary
  power management.
* Boot-image gotcha that outlives everything: header `page_size 4096`,
  kernel payload at offset 4096; verify ramdisk sha before packaging.

---

## 7. Sources (web, 2026-10-23)

* bentlybro — Mainline Linux on the Galaxy Tab S6 (SM8150 + Adreno 640 +
  GMU render-only on mainline, Aloha UEFI via magiskboot repack, kmsro +
  simpledrm display): https://bentlybro.com/posts/mainline-linux-on-the-galaxy-tab-s6-a-32-byte-field-and-one-line-of-device-tree
* Project Aloha platforms (SM8150 targets incl. samsung-beyond1qlte,
  samsung-winner, samsung-gts6lwifi): https://github.com/Project-Aloha/mu_aloha_platforms
* Mesa KGSL kernel-mode-driver backend (`freedreno-kmds`, freedreno Gallium
  and Turnip): Mesa release notes 24.1+ — https://docs.mesa3d.org/relnotes/24.1.0.html ;
  community kgsl-only build configs —
  https://github.com/DvaMishkiLapa/mesa-turnip-kgsl-build ;
  https://github.com/termux/termux-packages/issues/23907
* Adreno 640/650 upstream since Linux 5.8 (mainline drm/msm a640 maturity):
  https://www.phoronix.com/news/Adreno-405-640-650-Linux-5.8
* No existing d2q mainline/pmOS port (as of today) — closest are the Halium
  boots and the Exynos d2s early pmOS attempt; postmarketOS wiki has
  samsung-d1s (Note 10) only: https://wiki.postmarketos.org/wiki/Samsung_Galaxy_Note_10_(samsung-d1s)
