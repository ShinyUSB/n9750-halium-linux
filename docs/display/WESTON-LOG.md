# d2q Weston bring-up log (2026-09-29)

## Installed packages (Debian bookworm arm64, via apt over Wi-Fi)
- weston 10.0.1-1 (+ libweston-10-0, demo clients incl. weston-flower)
- wayland-utils 1.1.0-1 (wayland-info)
- libdrm-tests 2.4.114-1+b1 (modetest unusable: ignores -D on this box)
- kbd (earlier, for chvt; unused in the end)
- pre-existing: libwayland-client0 1.21.0-1

## Weston version / backend / renderer / device
- weston 10.0.1, `--backend=drm-backend.so`, `--use-pixman` (shadow fb),
  `--drm-device=card0`, `--seat=seat0 --tty=1 --continue-without-input`.
- No logind session over SSH (logind: "cannot find systemd session"),
  falls back to direct launcher as root. No seatd needed.
- XDG_RUNTIME_DIR=/run/user/0 (mkdir -p + chmod 700 each boot; /run is tmpfs).

## Kernel change required (out-fbcon rebuild -> image 60)
- SDE plane format lists contained DUPLICATE fourccs (e.g. AR24/XB24 twice
  per plane; verified live with a raw-syscall GETPLANE dumper).
- Weston 10 aborts: `weston_drm_format_array_add_format: Assertion
  '!find_format' failed` (drm-formats.c:131).
- Fix: `sde_populate_formats()` in sde_formats.c now dedupes globally
  (was consecutive-only). Writeback-configured modes path untouched.
- Verified live: 0 duplicates after flash.
- Image: artifacts/flash/60-halium-boot-fbcon-dedupe.img
  (dedupe kernel + no-sleeper ramdisk artifacts/display/ramdisk-fbcon-nosleeper.gz).
- Recovery stays 51-halium-boot-wlanfix.img (dab27f41...).

## Outputs / mode
- DSI-1 (conn 28): 1440x3040@60 preferred+current, crtc 133, QCM panel,
  73x155mm. wayland-info: logical 1440x3040 at (0,0), scale 1.
- Virtual-1 (writeback, conn 47): OFF via /root/.config/weston.ini
  ([output] name=Virtual-1, mode=off). Was 1024x768 at (1440,0).
- DP-1 disconnected.
- Note: wayland-info derives ~18Hz refresh from the CMD mode clock;
  cosmetic, presentation works (atomic flips complete continuously).

## Seat / VT / DRM ownership
- systemd-logind active, seat0 owns card0; no user sessions over SSH.
- fbcon/tty1 is the fallback: weston takes tty1 while running;
  SIGTERM restores the console cleanly (verified visually, no panel off).
- fb0/getty@tty1 intact; SSH + RNDIS + WLAN unaffected.

## Successful startup command (manual, over SSH)
```
mkdir -p /run/user/0 && chmod 700 /run/user/0
XDG_RUNTIME_DIR=/run/user/0 WAYLAND_DISPLAY=wayland-1 \
weston --backend=drm-backend.so --drm-device=card0 --use-pixman \
  --continue-without-input --tty=1 --seat=seat0 --log=/tmp/weston3.log
```
(Currently running as PID ~1321 with /root/.config/weston.ini.)

## Clients
- weston-flower: renders (static clover idle without input = expected).
- weston-presentation-shm: ANIMATES (verified visually).
- wayland-info: full protocol (compositor, xdg_output, shm, seat kbd+touch).

## Not done (explicit follow-ups)
- M6 input: seat reports keyboard+touch caps, untested (no input attached).
- M7 autostart: deliberately NOT a service yet; manual only.
- M8 GPU: renderD128 untouched; pixman only, no GL/EGL/GBM involved.
- M9 Xwayland: not enabled. M10 blanking: weston default idle 300s applies;
  clients killed after tests to avoid static content.
- modetest on this box ignores -D; used custom raw-ioctl dumper instead
  (sources not persisted; formats verified 0-dup live).

## Part B: autostart (2026-09-29) - WORKS
- Unit: artifacts/display/weston.service -> /etc/systemd/system/weston.service
  (Type=simple, TimeoutStopSec=15, XDG_RUNTIME_DIR + WAYLAND_DISPLAY env,
  --socket=wayland-1 --config=/root/.config/weston.ini, no --continue-without-input).
- Lessons, in order:
  1. getty@tty1 Type=idle defers its exec ~6s past boot; its vhangup(tty1)
     SIGHUPs Weston (two dead boots with 6s lifetime). Before= ordering
     made it worse.
  2. Conflicts=getty + ExecStopPost=start-getty deadlocks `restart`
     (stop-post TERMd by the conflicting start job).
  3. FINAL: getty@tty1 DISABLED at boot; Weston owns tty1 alone; standing
     fallback getty@tty2 (different VT, cannot vhangup Weston);
     ExecStopPost starts getty@tty1 after Weston dies (safe: Weston dead).
- weston.service enabled; drm-kick enabled; getty@tty1 disabled; getty@tty2 enabled.
- Cold reboot verified: Weston active, getty@tty2 active, getty@tty1 stopped,
  DSI enabled, RNDIS+WLAN+SSH up, desktop visible, flower reacts to touch.
- M10: SIGTERM exit restores tty1 (verified); 90s exit hang possible when a
  flip is in flight at teardown (SDE CMD may never deliver it) -> bounded
  by TimeoutStopSec=15, lastclose-restore recovers display regardless.

## Touch (2026-09-29) - WORKS, mapping correct
- sec_touchscreen /dev/input/event3 (i2c-13/13-0048, sec_ts): ABS_X/Y +
  MT slots 0-9, all 0..4095, no pressure/resolution. sec_touchpad event4,
  sec_e-pen event2 (rejected by libinput: no resolution), proximity event5.
- libinput: touch caps, seat0, identity calibration. Weston binds it to DSI-1.
- 5-point test (TL,TR,center,BL,BR): TL=(min,min), BR=(max,max), no mirror
  or rotation; edges within ~5%. No transform configured.
- weston-flower reacts to finger. Packages: libinput-tools, evtest.

## Idle-blanking and cursor (2026-10-01) - FIXED

Symptom: Weston turned the display off after ~5 minutes, and the cursor was not
visible.

Cause of the blanking: Weston's default `idle-time` is **300 s**. The deployed
`/root/.config/weston.ini` had only an `[output]` section, so the default applied.
Fixed by adding to `[core]`:

    idle-time=0      # 0 disables the idle timeout entirely
    cursor-size=48   # Weston 10 default is 32, too small on this panel

`weston.service` passes `--config=/root/.config/weston.ini`, so the file is read at
start; `systemctl restart weston` picks it up. Verified: log shows
`cursor planes: yes` and no config errors, and the desktop-shell panel keeps
rendering (idle-blanking gone).

Cursor themes are present (`/usr/share/icons/{Adwaita,default,hicolor}`, 124
cursor files), so no theme override is needed.

Note on verification: a 4x-subsampled /dev/fb0 dump appeared to show no cursor and
was misleading — subsampling can miss a thin/anti-aliased cursor. Do not conclude
"no cursor" from a subsampled framebuffer capture; use a full-resolution grab or
just look at the screen.

## Landscape / horizontal (2026-10-01) - WORKS, with pixman

The DSI connector exposes only portrait modes:
`1440x3040@18.1 (preferred/current), 1080x2280, 720x1520`. There is no landscape
panel mode, so landscape is produced by rotating the output in the compositor:

    [output]
    name=DSI-1
    transform=rotate-90

**The value is `rotate-90`, not `90`.** This is the whole trick. Weston 10.0.1
rejects a bare `90` (and `180`/`270`) with:

    Invalid transform "90" for output DSI-1

even though `weston --help` still advertises `normal 90 180 270 flipped ...`.
The help text is stale in this build. Values tested against the real DSI-1
output:

| value | result |
|---|---|
| `normal` | accepted |
| `90` / `180` / `270` | **rejected** |
| `flipped-90` | rejected |
| `rotate-90` | **accepted** |
| `flipped-rotate-90` | accepted |

**Correction:** it was *not* the pixman renderer blocking the transform. I
first assumed transforms required the GL renderer (they do in some versions) and
set up a GL test unit on that basis - that was wrong. `transform=rotate-90` works
with `--use-pixman` exactly as before. The GL test unit
(`weston-gl-test.service` + `weston-gl.ini`) was removed again; `weston.service`
is unchanged and still runs the pixman backend. No GL/llvmpipe regression.

Do NOT pass `--transform` on the command line either - this build reports
`fatal: unhandled option: --transform=90` and then fails to start
(`failed to create display: Broken pipe`), which takes the display down. The
config file is the only working route.

Verification: a full-resolution `/dev/fb0` grab shows the desktop-shell top panel
text rendered sideways, i.e. the whole output is rotated. Weston applies the
inverse transform to input, so touch and mouse follow.

Note: `/sys/class/graphics/fb0/virtual_size` stays `1440,3040` regardless - it
reflects the physical panel mode, not the compositor transform, so it is NOT a
valid check for whether the rotation took effect. Use a framebuffer grab.

## Persistence and rollback (2026-10-01)

Both display changes are on persistent storage and survive reboots with no
further action - nothing was applied as a runtime-only tweak:

- `/root/.config/weston.ini` - `idle-time=0`, `cursor-size=48`,
  `transform=rotate-90`. Read at every Weston start via
  `--config=/root/.config/weston.ini` in weston.service.
- `/etc/systemd/system/weston.service` - unchanged from the known-good pixman
  unit (the `--transform` experiment was reverted; the unit contains no
  `transform` at all, which is correct, because the CLI form kills Weston).
- `weston.service` is `enabled` and `active`.
- The GL experiment (`weston-gl-test.service`, `weston-gl.ini`) is deleted, and
  the device no longer has llvmpipe in the Weston path.

Authoritative copies live in this directory (`weston.ini`, `weston.service`) and
were diffed against the device to confirm they match byte for byte.

Rollback (portrait, no idle timeout change):

    sed -i '/transform=rotate-90/d' /root/.config/weston.ini
    systemctl restart weston

Old config backups on the device: `/root/weston.ini.bak-<timestamp>`.

## Cold-boot verification (2026-10-01)

After `reboot` with `weston`, `bt-mouse`, `bt-uart-pm` and `bt-uart-pm-guard`
all `enabled`:

- All four services `active`.
- `weston-info` reports **`output_transform: 90°`** on the `wl_output` -> the
  landscape transform is genuinely in effect, not just parsed. This is the
  authoritative check; a framebuffer grab of a mostly-black desktop is easy to
  misread, and `fb0/virtual_size` still shows the physical 1440x3040 panel.
- Zero `Invalid transform` lines in the Weston log.
- Bluetooth came up on its own: `hci0 UP RUNNING`, `acl:1286`, and the
  `Touch MM-997` input node was present without any manual action, i.e. the
  identity-address bond auto-reconnected the mouse.
