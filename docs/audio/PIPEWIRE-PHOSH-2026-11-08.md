# d2q PipeWire + Phosh session — 2026-11-08 (USER-TESTED: audio audible, lockscreen visible)

Device SM-N9750 (d2q) · Debian 12 · kernel 4.14.190-ShinyKernel #126.
Goal: desktop audio stack (PipeWire) + Phosh UI session, then user tests features.

## Installed (apt, bookworm)

pipewire 0.3.65 + wireplumber 0.4.13 + pipewire-alsa/pulse/jack + pulseaudio-utils;
phosh 0.24.0 + phoc + squeekboard 1.21 + feedbackd; upower, gnome-backgrounds,
grim, strace, systemd-coredump (later), libgbm-dev/libdrm-dev (for the probe).
New user `d2q` (uid 1000, groups audio/video/input/render/plugdev/users).

## Session architecture

- `phosh.service` (Debian unit, User=1000, PAMName=login, TTY tty7) — **enabled**,
  default target graphical.target. Drop-in `/etc/systemd/system/phosh.service.d/gpu.conf`:
  MESA_LOADER_DRIVER_OVERRIDE=zink, LIBGL_KOPPER_DISABLE=1,
  VK_ICD_FILENAMES=/opt/mesa-kgsl/share/vulkan/icd.d/freedreno_icd.aarch64.json,
  WLR_NO_HARDWARE_CURSORS=1, WLR_RENDERER=pixman.
- PipeWire trio runs in the user manager (user@1000, started by the PAM session);
  wireplumber rule `/etc/wireplumber/main.lua.d/51-d2q-audio.lua` pins the tavil
  ALSA nodes to volume 1.0 (ACP inits them at **17% = -46.5 dB**, inaudible — the
  "nothing played" bug).
- `d2q-xorg.service` DISABLED (rollback: `systemctl stop phosh; systemctl enable --now d2q-xorg`).
- d2q-audio-init (speaker route/amps) + d2q-mic-init (mic route) compose fine with
  WirePlumber's hw:0,0 stereo-fallback profile.

## Bugs fixed (chronological)

1. **/etc/environment had XDG_RUNTIME_DIR=/run/user/0** (from X bring-up) →
   user-environment-d generator leaked it into EVERY user service: pipewire
   tried /run/user/0 lockfiles ("Permission denied" → 243/CREDENTIALS), gnome-session
   found no user bus → silent rc=0 death. Removed (backup /etc/environment.bak).
2. **/dev/kgsl-3d0 was root:root 600** → uid 1000 couldn't open → zink
   vkEnumeratePhysicalDevices failed → pixman fallback. udev rule
   71-d2q-kgsl.rules (kgsl-3d0/kgsl-pwrctrl → group video 660).
3. **/dev/ion root:root 600** → the kgsl turnip fork needs /dev/dma_heap/system OR
   /dev/ion for VK_KHR_external_memory_fd ("TU: warning"). udev rule
   71-d2q-ion.rules (ion → video 660).
4. **Leftover gnome-session-binary held org.gnome.SessionManager** → new service
   gnome-sessions exited silently rc=0 → phoc restart loop. (comm is 15-char
   truncated — `pkill -x gnome-session-binary` does NOT match; use
   `pkill -f 'gnome-session-binar[y]'`.)
5. **Hardware cursor render loop**: sde cursor plane exposes no formats →
   "Failed to render cursor buffer" every ~2 s, each = full-screen pixman
   repaint = 100% CPU. Fixed with WLR_NO_HARDWARE_CURSORS=1.
6. **core_pattern pointed at systemd-coredump BEFORE it was installed** → every
   crash wedged phoc in kernel pipe_write (elf_core_dump → dead pipe reader).
   Installed systemd-coredump; set `/tmp/core.%p.%s` meanwhile.

## Audio end-to-end (USER-CONFIRMED audible)

paplay → PipeWire sink (hw:0,0/SEC_TDM_RX_0) → speakers → air → DMIC →
PipeWire source → pw-record: 1 kHz bin clearly above noise in the captured wav.
Sink+source at 100%, gains 60. Capture via `arecord -D default` goes through
PipeWire (pipewire-alsa) and works.

## GPU compositor status (OPEN — pixman for now)

- pixman session: STABLE, lockscreen visible, phoc idle ~2% CPU.
- GLES2/zink attempt: phoc SEGV inside the 25.3 fork's EGL (after ion/kgsl fixes;
  no debug symbols yet — needs gdb + phoc-dbgsym-style session).
- wlroots 0.16 (Debian phoc 0.24) has NO vulkan renderer ("Invalid WLR_RENDERER
  value: 'vulkan'" — vulkan renderer returned in wlroots 0.17+).
- Allocation forensics: gbm on this device can't load "msm": the fork is mesa
  25.3 with DRIL dispatch — the msm pipe driver was never compiled in
  ("msm: driver missing" from /usr/local/lib/aarch64-linux-gnu/gbm/dri_gbm.so);
  stock 22.3.6 msm_dri.so (restored via `apt-get install --reinstall libgl1-mesa-dri`)
  fails `get_param -6` — downstream msm DRM has no GPU ioctls (why the kgsl fork
  exists). Stock gbm failing → wlroots 0.16 would fall back to the DRM dumb
  allocator (dumb GEM + PRIME works downstream) with zink import — that's the
  path that segfaults. GPU-render follow-up = gdb the SEGV with
  MESA_DEBUG removed (it floods journald; phoc once hung blocked in pipe_write
  on a full stderr pipe).
- dri dir layout after the chase: /usr/lib/aarch64-linux-gnu/dri/msm_dri.so +
  swrast etc. = REAL Debian 22.3.6 files; zink_dri.so = symlink → /opt fork
  (re-pointed after the reinstall); /usr/local/lib/aarch64-linux-gnu/dri/* →
  /opt fork; /opt/mesa-kgsl/lib/dri/msm_dri.so symlink added (unused, harmless).

## Screenshot workflow

`XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 grim /tmp/x.png` (as root;
stale sockets block bind — if "no wl_output"/"failed to create display", check
`ls /run/user/1000/wayland-*` and clean stale ones after a restart).

## Open / bridges for user testing

- sensors: phosh auto-rotate needs iio-sensor-proxy; SLPI sensors here are NOT
  iio — needs the custom bridge (sensor-attach/ssc tools exist for probing).
- feedbackd haptics: no Linux vibrator device exposed (d2q `vib` script drives
  the motor directly) — bridge needed if apps must vibrate.
- NetworkManager absent → phosh "Error registering network agent" (benign);
  wifi stays on d2q-wlan.service. Install NM later only if wanted (may fight the
  manual wpa_supplicant setup).
- power key: d2q-powerkey.service disabled; logind/phosh handle power key via
  gpio-keys → test.
- brightness: gsd-power reads /sys/class/backlight/panel0-backlight — test in UI.

---
# SESSION 2 (2026-11-08 late): fail-whale root cause + WORKING session via wrapper

**"Oh no! Something went wrong" root cause = the Samsung kernel's mount_ns_option LEAKS the
proc-superblock s_umount** (every boot, within ~1 min): any NEW proc mount in any namespace
blocks forever in grab_super (D). Every dbus-activated sandboxed service (upower, hostnamed,
localed, rtkit...) then never starts → the unlock-triggered activation burst (battery widget →
upower) times out → gnome-session fail-whale → restart loop. Unrecoverable at runtime; only
sysrq-b. Mitigation (in place): shadowed unit FILES in /etc without namespace lines for
systemd-{hostnamed,localed,timedated}, upower, rtkit-daemon (drop-ins insufficient: systemd 252
ignores ProtectProc=no AND ProtectProc= from drop-ins); masked man-db/fstrim/e2scrub.
Verified: hostnamectl activates instantly.

**Second bug found the same way: phoc dies the moment a client draws** ("Error flushing
display: Broken pipe" in phosh 3 s after connect). Cause: gpu.conf exported the zink EGL env
(MESA_LOADER_DRIVER_OVERRIDE etc.) to the whole session — GTK rendered GPU dmabufs the pixman
compositor can't import. gpu.conf now carries ONLY WLR_NO_HARDWARE_CURSORS=1 + WLR_RENDERER=pixman.

**Third bug: the phoc -E "bash -lc gnome-session" child NEVER spawns under the systemd unit**
(silent; five-diag trail wrapped, cause unknown) — replaced by **/usr/local/bin/d2q-phosh-session.sh**
(via /etc/systemd/system/phosh.service.d/session.conf ExecStart override): rm stale sockets →
`WLR_BACKENDS=drm,libinput phoc -C ini &` (WAYLAND_DISPLAY must be UNSET for phoc or wlroots
picks the nested wayland backend and exits) → wait for the socket → export WAYLAND_DISPLAY →
exec gnome-session. systemd cgroup-kill cleans phoc on restart. First fully service-launched
session: gnome-session + phoc + phosh + squeekboard all up, shell RENDERS (clock, status bar,
app grid visible via grim).

Audio re-verified after the reboots: cleared poisoned wireplumber default-profile state,
combined output+input profile active, volumes 100%, tone loop loud (1 kHz bin 394k vs 224).
Backlight: d2q-backlight-assert didn't run on tangled boots — brightness was 2500/50000
("screen shows nothing"); assert manually with `echo 20000 > .../brightness` until the unit
ordering is fixed.

---
# SESSION 3 (2026-11-08 night): PHOSH/WESTON OUT, LABWC + LXQT IN (landscape)

User verdict on phosh: windows landed on the ghost output (Virtual-1), volume-up+power killed
the session → replaced wholesale.

- REMOVED: phosh, phoc, squeekboard, weston (apt) + weston.service custom unit deleted;
  phosh.service disabled. LXQt: lxqt-core installed.
- BUILT on-phone: **labwc 0.5.2** (github tarball; bookworm has no labwc; wlroots-dev is 0.15.1
  so the 0.6+ line is not buildable) → /usr/local/bin/labwc. meson 1.4 via pip (system meson
  1.0.1 too old? actually pip meson from the libssc work was used). **wlr-randr 0.2.0** built
  manually: needs the wlr-output-management XML fetched from wlroots 0.15.1 + wayland-scanner
  + `-lm` (Makefile-less tarball).
- TRAPS: labwc config flag is **-c NOT -C** (phoc habit; wrong flag = silent exit 0 + unit
  flap); wlr-randr 0.2 HANGS forever on transform/scale requests (labwc 0.5.2 never applies
  output-management configs → blocks the autostart script); **LIBGL_ALWAYS_SOFTWARE=1 must go
  in the autostart (clients), NOT the labwc environment file** — the environment applies to
  labwc too and software GL on the mixed dri stack (stock swrast_dri.so + fork libGL) SEGVs
  the compositor; Qt clients (lxqt-panel/pcmanfm) otherwise ABORT in the mesa-25.3 fork's
  egl_dri2 (`dri2_setup_device` assert with no render GPU).
- **LABWC PATCHES (source /root/labwc-0.5.2/src/output.c, rebuilt+installed):**
  (1) DSI-1 gets transform 90 + scale 2 baked in at output init (0.5.2 has no output config
  parsing) = LANDSCAPE; (2) **Virtual-1 ghost output DESTROYED on creation** (wlr_output_destroy
  in handle_new_output — the sway trick) — no more phantom screen for windows.
- Session unit: d2q-labwc.service (User=1000, PAMName=login, tty7, enabled, graphical.target)
  → /usr/local/bin/d2q-labwc-start.sh (same wrapper pattern: WLR_BACKENDS=drm,libinput, rm
  stale sockets, exec labwc). Autostart (~/.config/labwc/autostart): LIBGL_ALWAYS_SOFTWARE=1 +
  lxqt-panel + pcmanfm-qt --desktop + lxqt-notificationd + lxqt-policykit-agent. rc.xml:
  volume keys → pactl sink volume, XF86PowerOff → root menu; menu.xml: Terminal/Files/Settings/
  Reconfigure/Exit. Configs in /home/d2q/.config/labwc/ (host copies mnt-sda/audio-recon/labwc-conf/).
- State at handover: labwc + panel + desktop + notificationd all running service-launched;
  DSI-1 landscape only-output; grim canvas clean 3040-wide. KNOWN-OPEN: lxqt-panel bound to
  DSI-1 but not visibly painting in grim (suspect wlroots 0.15 layer-shell on transformed
  output) — panel may need floating config or 0.16 wlroots; desktop wallpaper black (default).
  Audio/PipeWire unchanged (independent of WM).

---
# SESSION 4 (2026-11-08 late night): wlroots 0.16.2 + labwc 0.6.2 — the fixes that mattered

User-reported: no start menu, touch not usable, pcmanfm not rendering, freezes. Root = the whole
wlroots-0.15 generation. REBUILT the stack on-phone:
- **wlroots 0.16.2** (gitlab.freedesktop.org archive — github codeload 404s for wlroots; repo lives
  on gitlab) — 90 s build. **labwc 0.6.2** against it (ldconfig needed post-install or exit 127).
- Wrapper env now: WLR_BACKENDS=drm,libinput + **WLR_RENDERER=pixman** (0.16 auto-renderer is
  GLES2-only then HARD FAILS — "msm: driver missing" from the fork EGL — no pixman fallback in 0.16
  auto; pixman+DRM dumb allocator is exactly right for this stack).
- wlr-randr --output DSI-1 --transform 90 --scale 2 now APPLIES (0.16 output-management works; it
  hung forever on 0.15) — landscape verified via wlr-randr (Transform: 90, Scale: 2.000000).
- Ghost-destroy patch re-applied to 0.6.2 new_output_notify (anchor: the server/wlr_output lines
  after the comment).
- **lxqt-panel needed layer-shell-qt** (apt package) — without it the panel is an xdg_toplevel
  buried under the desktop window. After install: panel paints, pcmanfm-qt opens, touch-driven
  navigation WORKS (user-verified in-session).
- Remaining cosmetics: pcmanfm desktop window gets a labwc titlebar behind the real window
  (windowRule serverDecoration in 0.6.2 rc.xml — my 0.5.2 rc.xml may need the 0.6 schema); panel
  layout config recreated fresh (weird fragment placement — user can re-arrange via Configure).

---
# SESSION 5 (2026-11-08 night 2): GPU COMPOSITING IS LIVE (Vulkan renderer on kgsl)

**The user's question "what do we need for GPU acceleration" answered and delivered.**

Stack: wayland 1.23 + wayland-protocols 1.34 + libdisplay-info 0.1.1 (bookworm caps at 1.21/1.31
and has no libdisplay-info — all built to /usr/local for wlroots 0.17) → **wlroots 0.17.4
(-Dbackends=drm,libinput -Drenderers=vulkan,gles2 + glslang-tools + hwdata)** → **labwc 0.7.4**.

wlroots 0.17.4 PATCHES required for the kgsl stack (all in /root/wlroots-0.17.4, rebuilt):
1. render/vulkan/vulkan.c `vulkan_find_drm_phdev`: kgsl VkPhysicalDevice has no
   VK_EXT_physical_device_drm → fall back to the first non-CPU device
   ("d2q: no DRM-associated Vulkan device; using fallback (kgsl)").
2. render/vulkan/renderer.c: `vulkan_open_phdev_drm_fd` fails ("missing both render and primary
   nodes") → reuse the backend's msm DRM fd (dup(drm_fd)) for scanout/dmabuf paths.
3. render/allocator/allocator.c: the gbm allocator SEGVs on this device (no msm pipe driver;
   stock msm_dri fd_drm can't init) → skip gbm unless D2Q_USE_GBM set; the DRM DUMB allocator
   then runs: "Created DRM dumb allocator", "Allocated 1440x3040 DRM dumb buffer".
   Vulkan renders into linear dumb buffers (DMA-BUF LINEAR ✓ texture ✓ render in turnip) and the
   sde scans them out — the Android-style dmabuf architecture, no GBM.
4. labwc 0.7.4 patches: `wlr_backend_is_drm` (declared-not-exported in 0.17.4) → libinput proxy in
   config/session.c; ghost Virtual-1 destroyed in new_output_notify (same as 0.6.2).

Wrapper env now: WLR_BACKENDS=drm,libinput WLR_RENDERER=vulkan
VK_ICD_FILENAMES=/opt/mesa-kgsl/share/vulkan/icd.d/freedreno_icd.aarch64.json
GBM_BACKENDS_PATH=/usr/local/lib/gbm-empty (belt&braces). labwc -d for debug logs in
/userdata/labwc.log (unit drop-in log.conf).

Desktop: **waybar** (replacing the X11-only lxqt-panel, which has no layer-shell in bookworm) with
MENU→wofi drun launcher, clock {:%H:%M}, battery, VOL; pcmanfm-qt desktop; wofi menu.
VERIFIED LIVE: start menu open over the desktop, icons, landscape 90@scale2, single output,
compositor ~0.5-2% CPU idle (vs 100% pixman). Client apps still software-rendered
(LIBGL_ALWAYS_SOFTWARE=1 in autostart — the fork EGL's egl_dri2 assert kills Qt's EGL probe);
next step for client GPU = fix/zink or dmabuf clipients.

---
# SESSION 6 (2026-11-08 deep night): KERNEL d2q-kick patch → image 308

Root cause of the wayland panel freezes (all compositor generations): this cmd-mode AMOLED
only latches a frame when an atomic commit "kicks" the DPU; the X-era solution was the
dirtyfb→d2q_kick workqueue (plane re-assert at TE rate, msm_fbdev.c). wlroots commits once
per frame and never kicks → panel holds the old frame until a burst. xrandr/grim/ssh stay
fine (compositor alive, commits ret=0).

Kernel patch (ShinyKernel-gpu-exp/Kernel, image 308):
- msm_fbdev.c: extracted `d2q_kick_arm(dev)` + `d2q_kick_active()`; set in-progress around
  the kick's own drm_atomic_commit (recursion guard); dirtyfb path now uses d2q_kick_arm.
- msm_drv.h: declarations under CONFIG_DRM_FBDEV_EMULATION.
- msm_atomic.c: after every successful userspace atomic commit, if !d2q_kick_active() →
  log + arm the kick (msm_atomic_commit is what the kick's commit re-enters, so the guard
  breaks recursion; D2QATM logging skipped for kick commits).
Build: bash /home/usb/.opencode/build-gpu.sh (distrobox kernel-build) — BUILD GOTCHA: the
fragment /home/usb/configs/d2q_a6xx_drm_experimental.config now carries
CONFIG_DSI_PARSER=y + CONFIG_DYNAMIC_DEBUG=y to match the RUNNING 307v2 config exactly
(without them the kernel is 48.5MB vs the 55.4MB reference and the pack flags header[9-11]).
Also: mnt-sda dropped its mount once mid-build (output reverted; log stranded) — keep build
logs on /home, recheck `df` before trusting an image.
Pack: pack-d2q.sh KERNEL=out-gpu/Image-dtb (55455042B, sha 75ad1de4) RAMDISK=307v2-extracted
(e0ca79b7) CMDLINE=307v2 REFERENCE=307v2 → artifacts/flash/308-halium-boot-d2qkick.img
(sha a607e7a4), check: "header identical except sha1 id; kernel is the only functional diff".
FLASH: heimdall flash --BOOT 308-halium-boot-d2qkick.img (user DL-mode dance). Rollback = 307v2.
NEXT after flash: freeze retest (expect gone); then touch matrix direction; then Xwayland
(SEGV after zink EGL init — MC) and client-side GL.

---
# SESSION 6 RESULTS (consolidated): kick experiments FAILED, rolled back to 307v2

- Image 308 (kick on every atomic commit, immediate): DEADLOCK — d2qkick worker stuck in
  sde_kms_prepare_fence→drm_modeset_backoff because the primary commit holds connection_mutex
  while waiting for panel events; the kick needs that same lock. Pipeline wedged until reboot.
- Image 309 (same, delayed 20ms + collapsed): SAME deadlock on the first collision — the
  delayed window still overlaps a commit; retry loop livelocks on an empty ww context.
- ROLLED BACK to 307-halium-boot-embpipe-v2.img (known-good). System stable again.
- LIVE EVIDENCE on 307v2 (freeze-watch.sh logging crtc_commit:133/187 stacks every 5s during
  real freezes): the commit kthread is ALWAYS cycling in
  sde_encoder_helper_wait_for_irq (_sde_encoder_phys_cmd_wait_for_idle / ..._wait_for_commit_done)
  — commits flow continuously, ZERO "ctl start interrupt wait failed" (no IRQ timeouts), zero
  D2QDIRTY beyond expected. So the sde commit machinery is healthy while the panel still
  shows stale frames → the stall is at the DSI cmd-mode latch/TE layer, NOT the commit path.
- New hypothesis (unverified): the X path worked because its dirtyfb flow produced a BLOCKING
  kick commit that waits one TE interval (~8-17ms measured in the 290-era work) — every frame
  got a TE-synchronized kick; wlroots' single async commit programs the DPU but the DDIC latches
  only at TE boundaries, dropping frames written mid-window. Kernel-side fix direction = TE-wait
  before DSI cmd kickoff (sde_encoder_phys_cmd) or a latch-count trace to prove the drop.
- Investigation notes: "pre link clk off cb failed" (dsi_clk_manager.c:880) occurs (2x/boot) but
  is LOG-ONLY (link clk disables anyway) — not proven causal.

---
# SESSION 7 (2026-11-08 night): BACK TO X11 — daily driver restored, wayland parked

Switched the boot default back to the proven X session (`d2q-xorg.service`, enabled;
`d2q-labwc.service` disabled). The X desktop = the old LXDE-style stack from the
`de.conf` drop-in: Xorg :1 (glamor/zink) + openbox + **lxpanel** + pcmanfm --desktop
+ xbindkeys (power-key toggle) + backlight assert. Touch works (X-era behavior: taps).

REGRESSION FOUND during the switch (important): today's `apt-get --reinstall libgl1-mesa-dri`
and `libgbm1` (from the GBM probing) REPLACED the mesa symlink farm:
- /usr/lib/aarch64-linux-gnu/dri/*  → real stock 22.3.6 files (msm/swrast/kmsro);
  zink_dri.so was re-pointed back to /opt manually.
- **/usr/lib/aarch64-linux-gnu/libgbm.so.1.0.0 → stock 22.3.6** (was a symlink to
  /opt/mesa-kgsl/lib/libgbm.so.1.0.0). Xorg SEGV'd instantly (glamor gbm → stock msm_dri
  → fd_drm get_param fails). FIX: `rm libgbm.so.1.0.0 && ln -s /opt/mesa-kgsl/lib/libgbm.so.1.0.0`.
  If Xorg ever dies at startup after package work, CHECK THE FARM FIRST.
- /userdata/gpuwork (Xorg -logfile dir) did not exist on this rootfs — recreated;
  without it Xorg exits 1 without writing its log.

Wayland stack left installed but disabled (labwc 0.7.4 + wlroots 0.17.4 vulkan, waybar/wofi,
touch calibrationMatrix, d2q-wl-keepalive; wrapper /usr/local/bin/d2q-labwc-start.sh;
freeze investigation state in SESSION 6 section + memory).

Minecraft/Prism (PineconeMC-Linux-aarch64.AppImage in /root): launches but its window stays
1x1 — the appimage waits on xdg-desktop-portal/secret-service (NoReply timeouts) in the bare
root X session. NEXT: run the launcher as user d2q (full user session: dbus+pipewire+
~/.local/share/ElyPrismLauncher instances) against DISPLAY=:1, or provide a minimal secret
service. GL path on X is proven (glamor/zink on kgsl).
