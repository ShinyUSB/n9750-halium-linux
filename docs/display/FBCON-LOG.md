# d2q fbcon console bring-up log

## 52 failure (2026-09-28, image 52-halium-boot-fbcon.img, NOT booted to Debian)
- Symptom: stuck on boot splash, no RNDIS, initramfs `halium_panic`
  ("Couldn't find data partition" path: mount/umount/tail/readlink/grep
  sequence at t=48.7s, then chvt + panic/telnet). Saved:
  host-side excerpt only (TWRP `/proc/last_kmsg` was NOT pulled then).
- Root cause: my cpio repack stored files as uid/gid 1000 (`usb`) instead
  of `root:root`. Original ramdisk: all `root:root`. Fixed method:
  `cpio -o -H newc -R 0:0`. Fixed ramdisk: `/tmp/opencode/ramdisk52-fixed.gz`.
- Kernel itself was fine (proven by 53 booting).

## 53 (fbcon kernel + PRISTINE 51 ramdisk, package-boot.sh): BOOTS.
- `/dev/tty0`, `/dev/tty1` exist; `getty@tty1` active (running agetty).
- No `/dev/fb0`; `Console: colour dummy device`; fbcon never bound.

## fbdev failure chain (drm.debug=0x1f, image 54)
1. `drm_setup_crtcs`: DSI-1 preferred 1440x3040 -> crtc 133 OK, BUT
   Virtual-1 (writeback connector) contributed phantom 4096x2160 ->
   combined fb 4096x3040 (~47 MB).
2. `msm_fbdev_create` -> `msm_gem_get_iova` -> -EINVAL (-22) -> fb destroyed.
- Fix 1 (sde_wb.c): `sde_wb_connector_get_modes` returns 0 when writeback
  has no configured modes (count_modes==0) instead of
  `drm_add_modes_noedid(4096x4096)`. Image 55: fb correctly 1440x3040
  (17.5 MB) but IOVA STILL -22 -> not size-driven.
- Root cause 2: SDE never assigns base `msm_kms.aspace` (only
  `sde_kms->aspace[]`), so `msm_gem_map_vma(NULL,...)` -> -EINVAL always.
  mdp4/mdp5 assign it; SDE was never wired for fbdev.
- Fix 2 (sde_kms.c, `_sde_kms_mmu_init`): `sde_kms->base.aspace = aspace`
  for `MSM_SMMU_DOMAIN_UNSECURE`. -> image 56 (pending).

## Kernel config fragment: configs/d2q_fbcon.config
CONFIG_VT=y, CONFIG_VT_CONSOLE=y, CONFIG_VT_HW_CONSOLE_BINDING=y,
CONFIG_DRM_FBDEV_EMULATION=y, CONFIG_FRAMEBUFFER_CONSOLE=y,
CONFIG_FONT_8x16=y, CONFIG_FONT_8x8=y
(olddefconfig also pulls DRM_KMS_FB_HELPER, FB_SYS_*, FB_DEFERRED_IO,
DUMMY_CONSOLE, FONT_SUPPORT, DETECT_PRIMARY=y.)

## Source patches (ShinyKernel/Kernel, files made u+w individually)
1. drivers/gpu/drm/msm/sde/sde_wb.c - no phantom wb modes when unconfigured.
2. drivers/gpu/drm/msm/sde/sde_kms.c - expose unsecure aspace via base hook.

## Images (artifacts/flash/)
- 52-halium-boot-fbcon.img cc51f4d4... DO NOT USE (bad ramdisk ownership).
- 53-halium-boot-fbcon-51ramdisk.img 4cb23a90... fbcon kernel, boots, no fb0.
- 54-halium-boot-fbcon-dbg.img be02d719... +drm.debug (current on device).
- 55-halium-boot-fbcon-wbfix.img 75657e0d... +wb fix, fb 1440x3040, IOVA -22.
- 56 (next): +aspace fix.
- Recovery: 51-halium-boot-wlanfix.img dab27f41...

## Ramdisks
- /tmp/opencode/img51/ramdisk.cpio = pristine 51 (ddcdc592...).
- /tmp/opencode/ramdisk52-fixed.gz = sleeper-disabled, root-owned (final use).

## fbcon bound but frozen (image 56/57)
- fb0 + `Console: switching to colour frame buffer device 128x48` OK,
  getty@tty1 running, but panel kept splash: NO atomic commit ever ran
  after fb registration (no dsi_display_enable in dmesg).
- fbcon_switch skips set_par when fb unchanged; boot takeover's set_par
  never committed. `lastclose -> msm_lastclose -> restore` DOES commit
  (proven twice: 56@93s via panel-dpms exit, 57 via cursor-test exit).
- Boot kicker: /usr/local/sbin/drm-kick (black legacy SETCRTC + instant
  exit; DSI auto-detect) + drm-kick.service (oneshot, Before=getty@tty1).
  Verified live + on cold reboot: splash -> console.
- 8 Tux penguins photo = per-CPU boot logo drawn by fbcon; text missing.

## CMD panel needs commits for updates (photo: logos only, no text)
- Panel is DSI CMD mode: frame only sent on atomic commit/kickoff.
- `echo HELLO > /dev/tty1` never appears: post-commit draws invisible.
- Root cause: msm_framebuffer_funcs has no .dirty, so
  drm_fb_helper_dirty() drops all fbcon damage (early return).
- Fix (msm_fbdev.c/msm_fb.c/msm_drv.h): .dirty = msm_fbdev_dirty(),
  which calls drm_fb_helper_restore_fbdev_mode_unlocked(priv->fbdev).
  -> image 58 (pending). Cursor blink will then commit ~2/s at idle.

## .dirty live updates (image 58/59) - WORKS
- msm_framebuffer_funcs had no .dirty: drm_fb_helper_dirty() early-return,
  all fbcon damage dropped (screen frozen at commit content).
- Added msm_fbdev_dirty() (restore path) + wired .dirty. Verified:
  `echo X > /dev/tty1` appears live; photo shows Debian login prompt.
- CMD panel only shows committed frames; cursor blink now commits ~2/s.

## Boot activation (drm-kick service) - WORKS
- fbcon never commits at boot on this stack (takeover's set_par path
  silent; VC-switch also skips set_par when fb unchanged).
- /usr/local/sbin/drm-kick (black legacy SETCRTC + exit, DSI auto-detect)
  + drm-kick.service (oneshot, Before=getty@tty1): process exit drops last
  DRM ref -> msm_lastclose -> restore commits fbcon. Cold reboot verified:
  splash -> console, survives, getty active.
- Image 59 = 58 kernel + fixed no-sleeper ramdisk (--owner=0:0). No 90s
  steal/restore cycle anymore. FINAL-59-CHECK verified live on screen.

## Notes
- Replacement screen: user reports scaling looks off; orientation/brightness
  tuning is follow-up work, not a blocker.
- `apt install kbd` pulled chvt (unused in the end); console-setup postinst
  errored (cosmetic, font setup) - check `dpkg --configure -a` later.
- Local keyboard input (USB OTG) untested: getty runs, prompt visible,
  but no input device attached yet. Next milestone: input + local login.
