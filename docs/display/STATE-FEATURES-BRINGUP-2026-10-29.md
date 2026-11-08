# FEATURE BRING-UP SESSION — 2026-10-29 (cameras / accelerometer / GPS / S Pen)

Device: SM-N9750 (d2q, China variant, **Snapdragon 855 / SM8150**, Adreno 640) — NOT Exynos.
Base state at session start: image 300 (`300-halium-boot-asyncdirty.img`) running, display path done,
Xorg :1 + LXDE, GPU stack done.

**HEADLINE RESULT: image 301 built and PACKED (`301-halium-boot-penres-rmtfs.img`, sha256
`fc945b7e98677896…`, kernel Image-dtb sha `2f4534c384eb5a74…`) = image 300 kernel + TWO additive
changes: (1) Wacom W9020 digitizer axis resolution (libinput tablet fix), (2) `CONFIG_UIO_MSM_SHAREDMEM=y`
(Samsung sharedmem-uio driver → `/dev/qcom_rmtfs_uio0` for the RMTFS daemon). Header diff vs 300:
id-field only (pack-d2q.sh REFERENCE check passed, ramdisk sha eb5d2eb5 canonical, cmdline = 300's
454-byte cmdline). AWAITING USER DL-MODE FLASH.**

## What was found and what works NOW (no flash needed)

### S Pen — WORKING IN X TODAY (image 300)
- The digitizer is a **Wacom W9020** on I2C bus 11 @ 0x56, driver `drivers/input/wacom/wacom_i2c.c`
  (`CONFIG_EPEN_WACOM_W9020=y`), input name `sec_e-pen` (event2). It registers proper tablet caps:
  BTN_TOOL_PEN/RUBBER, BTN_TOUCH, BTN_STYLUS, ABS_X 0–7344, ABS_Y 0–15504, ABS_PRESSURE 0–4095,
  ABS_DISTANCE, ABS_TILT_X/Y, SW_PEN_INSERT. Live evtest capture (/userdata/pen-cap.txt, 1.2 MB)
  confirmed real pen motion + pressure while the user was drawing.
- **libinput REFUSES the device** (`X11.log`: "libinput bug: missing tablet capabilities: resolution.
  Ignoring this device") because the kernel driver never sets axis resolution.
- TODAY'S WORKAROUND (already deployed, no reflash): `xserver-xorg-input-wacom` 1.1.0 installed +
  `/etc/X11/xorg.conf.d/70-spen.conf` (MatchProduct "sec_e-pen" → Driver "wacom"). After
  `systemctl restart d2q-xorg`: **`sec_e-pen stylus` (id=6) and `sec_e-pen eraser` (id=12) appear in
  xinput** with full tablet area 0,0,7344,15504. Pen + pressure usable on the desktop.
- KERNEL FIX (in image 301): `input_abs_set_res(ABS_X, max/70)`, `input_abs_set_res(ABS_Y, max/155)`
  after the xy_switch block in `wacom_i2c.c` (input_set_abs_params area, ~line 1774). After flashing,
  the native libinput tablet path works; the 70-spen.conf wacom-driver override can be REMOVED to
  test the libinput path (keep the file around as fallback).

### Camera stack — fully identified (capture = future work)
- Kernel side is Qualcomm **CamX/Spectra** (`CONFIG_SPECTRA_CAMERA=y`): /dev/media0 = cam_req_mgr,
  /dev/media1 = cam_sync, plus 28 v4l-subdevs. Entities: cpas, isp, **csiphy×4**, **6× cam-sensor-driver**
  (v4l-subdev9..14), actuator×3, flash×3, eeprom×5, ois, aperture, icp, jpeg, fd, lrme.
- Sensor identities from Samsung camera sysfs (`/sys/class/camera/{rear,front}/…`):
  **rear main = SONY IMX345** (OIS, COREVOLT=Y), **SLSI S5K3M5** (OIS=Y — tele), **SLSI S5K3P9**
  (ultrawide), **front = SONY IMX374**. CCI i2c: bus 9 (eeprom 9-0001/9-0003, one camera client
  9-0006, actuator 9-0018), bus 10 (aperture 10-004c, OIS 10-0051). Most sensor I2C clients appear
  only at CAMON (Samsung power-sequences cameras lazily).
- Getting actual FRAMES requires a CamX userspace (the kernel UAPI is session-based: cam_req_mgr
  links, CSID/IFE config via cam_isp.h ioctls). NOT attempted this session — this is the big
  roadmap item. libhybris + Android camx HAL is the pragmatic long path; hand-rolled CamX UAPI
  client is the hard path.

### Accelerometer (and the whole sensor suite) — DSP booted, data path scoped
- Accelerometer = **STM LSM6DSO behind Samsung SSC on the SLPI DSP** (NOT direct I2C from the AP):
  `CONFIG_SENSORS_SSC=y`, `drivers/sensors/sensors_ssc.c` + `drivers/adsp_factory/adsp_factory.c`.
  /sys/class/sensors/{accelerometer,gyro,magnetic,barometer,light,proximity,grip}_sensor exist,
  LSM6DSO + STM vendor strings, raw_data present but `0,0,0`.
- **SLPI boots successfully**: `echo 1 > /sys/kernel/boot_slpi/boot` →
  `subsystem_get_with_fwname("slpi","slpi_spu")` (SPU variant, falls back to "slpi" since the
  spu partition is empty) → subsys-pil-tz loads slpi.mdt → **SLPI ONLINE**, fastrpc rpmsg channel
  opens (`adsprpc: opened rpmsg channel for slpi`). boot_adsp/boot_cdsp kobjects exist too.
- Firmware location SOLVED: stock PIL firmware lives on the **apnhlos partition** (mounted at
  `/vendor/firmware-modem` — one of the kernel's firmware search paths): adsp.mdt/b*, cdsp,
  **slpi.b00-b20+slpi.mdt**, modem.b*+modem.mdt all there. `/dsp` partition = fastRPC shell modules;
  `/spu` partition = all zeros (never provisioned — explains `boot_slpi/cmd_result` = `fw_update,4:NG`).
- Actual sensor VALUES need the Samsung SSC userspace: the SSC char dev (`/dev/sensors`) is only a
  timer stub; the HAL talks **fastRPC to SLPI** (libsensors via adsprpc). Next step: andersson/fastrpc
  userspace in the arm64 chroot + speak the SSC client protocol (or libhybris sensors HAL).

### GPS / modem — root-caused to RMTFS; fix staged in image 301
- The modem (and every DSP) never boots on Debian because nothing triggers PIL and the fw paths were
  missing. BOTH solved this session:
  - Boot trigger: **open() of `/dev/subsys_<name>` performs subsystem_get** (subsystem_restart.c
    fops). Holding the fd open keeps it up. Also `/sys/kernel/boot_{slpi,adsp,cdsp}` write-knobs.
  - Firmware: apnhlos mount (see above); kernel fw_path already includes /firmware/image,
    /vendor/firmware-modem/image, /spu/sensorhub.
- **Modem boots in ~0.5 s** (`echo`-free: open /dev/subsys_modem) and QRTR comes alive — the modem
  node is **qrtr node 0** (HELLO logged as "Modem QMI Readiness RX cmd:0x2 node[0x0]").
  `qmicli -d qrtr://0` reaches the modem's QMI CTL (nodes 2-6 = "not found", node 1 = AP).
  qmicli 1.32.2 + xinput installed on the phone (apt over WiFi).
- **BUT the modem crash-loops without RMTFS** (crash_count reached 28: boots, fails to mount EFS,
  `modem: Ignoring error fatal, restart in progress`). Stock `/vendor/bin/rmt_storage` is a bionic
  binary — unusable on glibc Debian. DMS and LOC client creation both fail `Internal` while the
  modem is in this state (QMI CTL itself answers).
- FIX STAGED: the stock DT already has `qcom,rmtfs_sharedmem@0 { compatible = "qcom,sharedmem-uio";
  qcom,client-id = <1>; }` and the kernel tree ALREADY CONTAINS `drivers/uio/msm_sharedmem/` — it was
  simply `# CONFIG_UIO_MSM_SHAREDMEM is not set`. Image 301 enables it → `/dev/qcom_rmtfs_uio0`.
  `andersson/rmtfs` (built in the arm64 chroot, dynamic link: libudev.so.1 only — repo Makefile's
  -lqrtr satisfied by bundling qrtr-lib's qrtr.c/logging.c/qmi.c; libudev-dev needed apt in chroot)
  is deployed to `/usr/local/bin/rmtfs` with storage symlinks:
  /boot/modem_fs1→modemst1, modem_fs2→modemst2, modem_fsc→fsc, modem_fsg→fsg (by-partlabel).
  Run WITHOUT -P (uio path). rmtfs serves the modem EFS over QMI RMTFS on qrtr.

### Persistence (systemd units, all enabled, staged for post-flash boot)
- `d2q-stockmounts.service` — ro-mounts modem→/firmware, apnhlos→/vendor/firmware-modem, dsp→/dsp,
  vendor→/vendor (idempotent mountpoint guards).
- `d2q-qrtr-ns.service` — `/usr/local/bin/qrtr-ns` (built from pre-drop andersson/qrtr, static).
  NOTE: on THIS boot an unknown holder already binds the QRTR ctrl port (EADDRINUSE → our ns goes
  "dormant"), yet lookups WORK (libqmi finds services), so something answers the ctrl port; identity
  unresolved. After reboot the unit provides the NS regardless.
- `d2q-rmtfs.service` — rmtfs daemon (Before=d2q-modem).
- `d2q-modem.service` — `exec tail -f /dev/null 3</dev/subsys_modem` (open-keeps-modem-up holder).
- `d2q-slpi.service` — oneshot `echo 1 > /sys/kernel/boot_slpi/boot`.

## POST-FLASH VERIFICATION CHECKLIST (image 301)
1. `cat /proc/cmdline` unchanged; kernel = `2f4534c3…` (Image-dtb sha).
2. `/dev/qcom_rmtfs_uio0` exists; `systemctl is-active d2q-rmtfs d2q-modem` both active;
   `cat /sys/bus/msm_subsys/devices/subsys0/state` = ONLINE and STAYS online (crash_count low).
3. `qmicli -d qrtr://0 --loc-noop` → client created. Then:
   `qmicli -d qrtr://0 --loc-set-operation-mode=standalone` + `--loc-start` +
   `timeout 60 qmicli -d qrtr://0 --loc-follow-nmea` (client-cid reuse via --client-no-release-cid).
   OUTDOOR for an actual fix; GSV/GGA traffic indoors still proves the GNSS engine runs.
4. Pen native path: remove /etc/X11/xorg.conf.d/70-spen.conf, restart d2q-xorg, check
   `libinput list-devices` marks sec_e-pen a tablet (resolution now present). Restore file if worse.
5. SLPI boots via d2q-slpi (sensors class alive, raw_data still 0,0,0 until the fastRPC client exists).

## Open items / roadmap
- **Accelerometer values**: fastRPC userspace (andersson/fastrpc) + Samsung SSC client protocol on
  SLPI; or libhybris sensors HAL. Grip (SX9360) + ALS (tcs3407) are direct-I2C AP-visible devices —
  simpler fallbacks.
- **Camera capture**: CamX UAPI userspace (big) or libhybris camera HAL. Sensor subdev↔model mapping
  still to be done at CAMON time.
- **Modem after RMTFS**: DMS/NAS/UIM services should come up (calls/SMS later); pd-mapper/tqftpserv
  NOT needed for basic QMI on sm8150.
- **Finger-touch drag still broken** (sec_ts reports taps only — event3/4, C01T0122.LCIA.L fw) —
  separate dig, unchanged this session.
- qemu-user chroot gained: libudev-dev; /root/{rmtfs,qrtr-lib,qrtr-ns} build trees. Host staging:
  /home/usb/mnt-sda/{rmtfs,qrtr-src,qrtr-ns-old}.

## POST-FLASH RESULTS (image 301 flashed 2026-10-29 ~19:20, same session)

- **FLASH: clean.** heimdall 2.2.2, DL 04e8:685d, boot ~20 s, boot_id 782c5aa4.
- **S Pen native resolution: CONFIRMED.** `libinput list-devices` now reports
  `sec_e-pen … Capabilities: tablet, Size: 71x155mm`. HOWEVER Xorg's libinput still binds the
  device as a KEYBOARD (id=6 slave keyboard) even though the CLI libinput sees a tablet — cause
  not yet dug (no X log found at the usual paths). **The wacom-driver override
  (/etc/X11/xorg.conf.d/70-spen.conf → Driver "wacom") remains the working path and is restored**:
  `sec_e-pen stylus` + `sec_e-pen eraser` present, pressure OK. Native-libinput switch = small
  follow-up.
- **Mount-order trap (cost 30 min):** d2q-stockmounts must mount **/vendor (sda25) FIRST**, then
  apnhlos at /vendor/firmware-modem — mounting apnhlos before /vendor leaves the submount SHADOWED
  under the later /vendor mount (`ls /vendor/firmware-modem/image` = ENOENT even though `mountpoint`
  claims it is one). Unit file fixed + redeployed. Symptom when wrong: SLPI/modem firmware lookups
  fail (`firmware slpi.mdt: request_firmware wait timeout rc=-2`).
- **sharedmem-uio probed**: /dev/uio0 with name "rmtfs" (maps/map0 addr 0xf8701000 size 0x200000).
  rmtfs expects /dev/qcom_rmtfs_uio1 — Android creates it via udev; our shipped rmtfs.rules only
  matches qcom_rmtfs_mem. Manual symlink + ExecStartPre in d2q-rmtfs (deployed) fixes it; the udev
  rule /etc/udev/rules.d/99-rmtfs-uio.rules did NOT fire (ATTR{name} match on uio — untested debug).
  With the symlink, **rmtfs runs and maps the region** but…
- **MODEM BLOCKED (the one open dig): crash-loops ~40 s per boot** (`Fatal error on modem!`,
  crash_count 23+), and rmtfs receives ZERO requests. Established facts: modem = qrtr node 0, its
  QMI CTL is reachable from the AP (qmicli worked on the 300 boot; on 301 not re-tested during a
  live window); rmtfs binds node1:port 0xe and sends NEW_SERVER to the local ctrl port; lookups FROM
  node 1 reach the modem's own NS (routed, dst_node=0). The modem can NOT discover node-1 services:
  its lookups for rmtfs would be sent to node-1's ctrl port, where an UNIDENTIFIED holder lives
  (EADDRINUSE for QRTR_PORT_CTRL since boot; no userspace process holds it; net/qrtr/fifo.c ruled
  out — it is the SPU-microvisor transport, and the SPU VM IS alive: it announces service 0x1001:*
  bursts visible in /sys/kernel/debug/ipc_logging/qrtr_{0,9}). qrtr-ns (userspace) can never bind →
  restart-looping → disabled (it also HELLO-spams the modem). Direct NEW_SERVER to node-0 ctrl from
  python = ENODEV (fork's send rules). IPC logs of the modem's glink endpoint: not found (only the
  FIFO contexts exist). ALSO OPEN: Samsung's rmt may be **memshare-based** (stock rmt_storage uses
  msm_memshare — "memshare: Modem shutdown" seen) rather than QMI-RMTFS; if so andersson/rmtfs is
  the wrong protocol entirely — strings the vendor binary (scp to host; no `strings` on phone) as
  the first step next session. Next-session plan: (1) host-strings /vendor/bin/rmt_storage,
  (2) find/neutralize the ctrl-port holder (grep idr/PORT_CTRL in qrtr.c, consider a kernel debug
  patch to log the holder), (3) if QMI-RMTFS is right, patch qrtr to answer node-1 lookups for
  service 0x0e statically or make forward rules deliver NEW_SERVER cross-node.
- qrtr-ns unit DISABLED (leave off until the ctrl-port holder question is settled).
- SLPI boots fine once mounts are ordered (re-verified ONLINE, fastrpc channel up).

## SESSION 2 (same day, evening) — MODEM STABILIZED, GPS LAST-MILE MAPPED

**MODEM IS STABLE NOW (crash loop FIXED):** crash_count frozen at 149 since the
fsg/rmtfs/NS fixes. The recipe that made it work (all userspace, no reflash):
1. `rmtfs` (andersson) patched: `fd_open` falls back to O_RDONLY on EROFS/EACCES —
   **Samsung write-protects `fsg` in HARDWARE** (it lives on /dev/sdd, a read-only UFS LU;
   modemst1/2/fsc are on sda and are rw). Binary rebuilt in the arm64 chroot, deployed
   /usr/local/bin/rmtfs (d2q-rmtfs unit).
2. **`/usr/local/bin/qrtr-ns-lite.py`** (python, source also at /home/usb/mnt-sda/) — the NS that
   replaces qrtr-ns. WHY IT'S NEEDED: Samsung's `qrtr_sendmsg` **silently drops userspace
   NEW_SERVER announcements to the local ctrl port** (the `qrtr_must_forward` same-net drop
   branch in qrtr.c: ~1690), so NO daemon can register services the stock way, and nothing
   ever answered the ctrl port. ns-lite binds the local ctrl port (free once d2q-qrtr-ns is
   disabled), CACHES every NEW_SERVER (the modem announces its whole QMI stack at boot),
   answers lookups from cache + a STATIC entry (rmtfs = svc 0x0e on node 1 port 0xe), and
   re-announces static services to the modem every 5 s.
3. With EFS served, the modem announces its full QMI service stack (WDS/DMS/NAS/WMS/UIM/
   VOICE/...). d2q-qrtr-ns must stay DISABLED (its restart loop HELLO-spams the modem).

**GPS LAST MILE (blocked, precisely mapped for next session):**
- LOC = **QMI service 0x10 (inst 0x2, node 0 port 0x47)** — confirmed announced (and the
  announced list is complete in ns-lite's cache; no separate CTL service is ever announced).
- qmicli can't be used yet: libqrtr-glib's "initial bus lookup" asks for service 0 (CTL);
  the modem never announces CTL and does NOT answer lookups itself; ns-lite answered the
  CTL lookup from cache — but there IS no CTL service on this modem to point at.
- Raw LOC requests to (0,0x47) transmit (`qrtr_0_tx` IPC log: `[python3]W: Len:0x7 dst[0x0:0x47]`)
  but the modem NEVER replies, and retries ETIMEDOUT (`qrtr_tx_wait` waits for RESUME_TX that
  never comes; flows stay stuck until the modem restarts). Asymmetry: **modem-initiated flows
  (RMTFS) work bidirectionally; AP-initiated flows die after packet 1.** Next dig = kernel
  qrtr fork's confirm_rx/RESUME_TX semantics + whether the SPU-FIFO endpoint stole node 0 in
  the radix tree (ports 0x1/0x2 answered 0x21 with result-OK — possibly the SPU VM, not the
  modem; both endpoints register with NID_AUTO and the first packet claims the id).
- Also unresolved: why the modem never announced/started an LOC *session* service — maybe LOC
  inst 0x2 is a stub and the real one starts on demand via the Samsung loc_api HAL.

**Kernel source landmarks for the next dig** (net/qrtr/qrtr.c):
- `qrtr_node_enqueue` ~557: hello gate (`!hello_sent && type != HELLO → -ENODEV`), tx_wait.
- `qrtr_tx_wait` / RESUME_TX handling: flows keyed (node,port), stuck pending → ETIMEDOUT.
- `qrtr_sendmsg` ~1690: the NEW_SERVER same-net DROP branch (the reason ns-lite exists).
- `qrtr_node_assign` ~663: NID_AUTO claim on first non-HELLO packet (node-0 collision risk).

## SESSION 3 (same day, late evening) — QMICLI FULLY WORKING, LOC SESSION STARTS

**THE ENTIRE QMI TOOLCHAIN NOW WORKS on the phone.** `qmicli -d qrtr://0 --dms-get-manufacturer`
returns `Manufacturer: 'QUALCOMM INCORPORATED'`. The three pieces that were missing:

1. **libqrtr-glib's "initial bus lookup" protocol decoded** (from libqrtr-glib/src/qrtr-bus.c,
   fetched from gitlab.freedesktop.org/mobile-broadband/libqrtr-glib, branch `main`): the client
   sends NEW_LOOKUP (svc 0, inst 0, all-zero fields) to the LOCAL ctrl port; this is a DUMP-ALL
   request; the bus **finishes initializing only when it receives an all-zero NEW_SERVER
   terminator packet** (node=0,port=0,service=0,version=0,instance=0). Without the terminator it
   times out no matter how many servers you send. ns-lite now: svc-0 lookup → dump the whole
   cache + STATIC + **terminator**; specific lookups → matching entries + terminator.
   (Also: the lookup ALWAYS goes to the LOCAL ctrl port regardless of the qrtr://N URI.)
2. **qmi-fake-ctl.py** (`/usr/local/bin/`, deployed): the Samsung modem NEVER announces a CTL
   service, and libqmi needs one to allocate clients. The fake CTL binds node1:0x99, answers
   get-version-info / allocate-client (always client 1) / release. ns-lite STATIC now includes
   svc 0x00 → (1, 0x99). Client ids are cosmetic — over qrtr the service identifies clients by
   source port.
3. **The LOC silence mystery RESOLVED**: the LOC service was alive all along. The "no reply" was
   my own probes having burned the flow-credit window (QRTR_TX_FLOW_HIGH=10, LOW=5 — 10
   outstanding un-ACKed packets per (node,port) flow block sends with ETIMEDOUT until the modem
   restarts). First-touch sends work; never probe-scrape a service port. DMS/NAS/LOC all answer
   instantly on first touch (DMS returns "QUALCOMM INCORPORATED").

**QMI LOC SESSION PROVEN**: raw QMI LOC_START (msg 0x22, TLV 0x01 session id) to (0,0x47) returns
**result SUCCESS**; `qmicli --loc-set-operation-mode=standalone` works ("Successfully set operation
mode"); `--loc-start` works ("Successfully started location tracking"). SET_NMEA_TYPES (0x26/0x27)
rejected with error 0x39 — id or trimmed service, unresolved, cosmetic.
**Gotcha**: qmicli allows ONE LOC action per invocation, and the LOC session DIES when the owning
client's socket closes — a `--loc-start --client-no-release-cid` background qmicli still exits
after the action, killing the session. Use `/root/gps-hold.py` on the phone (raw python: starts a
session and listens ~15 min, logging 0x20 position reports + 0x24 NMEA to /userdata/gps2.log).
No position reports yet indoors (no fix) — **needs sky time: run gps-hold.py with the phone at a
window/outdoors; a fix lands as msg 0x20 in the log.**

**Persistence updated**: d2q-qrtr-ns DISABLED; new units d2q-qrtr-ns-lite + d2q-qmi-fake-ctl
(enabled, Restart=always, Before=d2q-modem). After a reboot the full QMI stack comes up
automatically. Sources: /home/usb/mnt-sda/{qrtr-ns-lite.py,qmi-fake-ctl.py}; phone
/usr/local/bin/{qrtr-ns-lite.py,qmi-fake-ctl.py}. **mnt-sda unmounted itself AGAIN mid-session
(a stray pd-mapper clone briefly landed on the underlying /home disk at 100%) — remounted
/dev/sda2; check `df /home/usb/mnt-sda` before writing to it.**

## SESSION 3 addendum — post-reboot GPS behavior (user question: "manual bring-up needed?")

**NO manual bring-up needed**: after a full power-off + reboot the whole QMI stack auto-starts
(all units active, modem ONLINE, crash_count 0, EFS served). Two script-level gotchas bit:

1. **The LOC service port MOVES between boots** (0x47 → 0x4a observed). gps-hold.py now resolves
   it dynamically: QRTR NEW_LOOKUP svc 0x10 to (1, ctrl) → ns-lite answers from cache + terminator
   → take the (node,port) from the NEW_SERVER reply.
2. **Standalone operation mode is VOLATILE** — must be re-set after every boot (default MSB fails
   session start). gps-hold.py now shells out to `qmicli --loc-set-operation-mode=standalone`
   first. (Old failure signature: LOC_START result=FAILURE err=0x3a; and hitting a foreign
   service on a stale port also yields an error response — both look similar.)

gps-hold.py (phone /root/, host /home/usb/mnt-sda/gps-hold.py) now: resolves LOC port → sets
standalone → starts session (result must be 0, else ABORT with reason) → listens 14 min →
decodes fixes ("FIX: lat=… lon=…" from TLV 0x01/0x02 i64 deg*1e7). Sky time still required:
no reports indoors.

## SESSION 4 (2026-10-30) — GNSS ENGINE PROVEN ALIVE (indoor), correct protocol tables

**The GNSS engine runs, holds sessions, streams NMEA + SV info + engine-state events indoors.**
No fix yet (GPRMC stays `,,V` and GET_GNSS_SV_INFO errors 0x02 — zero sats visible indoors on this
phone). Position lock = the next outdoor window (see "outdoor test" below).

**Corrected LOC protocol ids** (libqmi's qmi-service-loc.json, data/ dir, branch main — the earlier
session used qmicli's mixed indices; use THESE from now on):
- Register Events **0x0021** (TLV 0x01 mask u64; bits: 2=position report, 5=NMEA, 8=engine state —
  0x3FFFFFF enables everything; response = SUCCESS). THE MISSING STEP earlier: without it the engine
  sends NOTHING after START.
- Start 0x0022, Stop 0x0023. Set Operation Mode 0x004A. Set/Get NMEA Types **0x003E/0x003F** (the old
  0x26/0x27 in the scripts were wrong — hence error 0x39; correct SET returns result=0).
- Indications: Position Report **0x0024** (TLV 0x01 session-status u32 — 0=SUCCESS, 1=IN_PROGRESS,
  4=NO_FIX; 0x10/0x11 lat/lon as gdouble; 0x12 uncertainty float; 0x2B fix count), GNSS Sv Info
  0x0025, NMEA 0x0026, Inject Time Request 0x0028, Inject Predicted Orbits 0x0029, Inject Position
  0x002A, Engine State 0x002B (gint32; 1 seen = ON), Fix Recurrence 0x002C, Inject UTC 0x002D.
- GET_SERVER (0x0043) → error 0x11 (no XTRA server config) — assistance-data path not provisioned.

**Decoded stream (indoors, /userdata/gps2.log)**: engine state 1 (ON); inject-time request carries
its XTRA URL (path2.xtracloud.net); NMEA is ~1 Hz $PQW*/$PQWP* proprietary status + an empty GPRMC
(`V` = no fix); SVINFO indications fire at 1 Hz (1 list entry = the header; the GNSS-SV-info QUERY
errors 0x02 = no sats). **NMEA types register reads 0 after a successful SET (Samsung quirk —
setting is persisted in the session but reads back 0).** gps-hold.py v2 deployed (phone /root/,
host /home/usb/mnt-sda/gps-hold.py): resolves the LOC port (moves each boot), sets standalone,
registers events, decodes everything with status names; arg = minutes.

**Outdoor test**: `bash /home/usb/.opencode/u.sh 'nohup python3 /root/gps-hold.py 25 > /dev/null 2>&1 &'`
then take the phone outside; log lands in /userdata/gps2.log (FIX lines = POSREPORT status=SUCCESS
with lat/lon; NMEA GPRMC flips from `V` to `A`).

## SESSION 4 addendum — outdoor test + assistance state (2026-10-30)

- **5-min outdoor walk**: no fix — POSREPORT status=TIMEOUT (engine searched, found nothing), GPRMC stayed V, SVINFO empty. Indoor/outdoor identical engine state — antenna/RF marginal or time/position starved.
- **Assistance probed**: GET_PREDICTED_ORBITS_DATA_VALIDITY (0x37) = VALID with XTRA URL (path2.xtracloud.net). GET_SERVER (0x43) err 0x11 (no XTRA server cfg). SET_NMEA_TYPES (0x3E) result 0 but reads back 0 (Samsung quirk). UTC/POS inject (0x38/0x39) err 0x11 (rejected). DELETE_ASSISTANCE_DATA (0x44, FFFFFFFF) succeeded; fresh session started.
- Next lever if no fix in clear sky: Samsung GNSS RF/antenna-path control (LNA gate / AGC state in $PQW stream), not protocol. A fresh 10-min gps-hold.py window was running at session end.

## SESSION 5 (2026-10-30) — GNSS RF dig: protocol done, acquisition = hardware window

**Verdict: the GNSS *stack* is fully working; the *receiver* simply isn't seeing satellites in any
of our test windows.** Established this session:
- **hotdog (OnePlus 7T, same SM8150, mainline 6.16)** — its GNSS evidence file (/home/usb/
  2026-08-10-gnss-qmi-loc.md) shows the EXACT same shape: QMI LOC works (sessions start/stop,
  NMEA caps), and "no satellite was acquired" indoors with the session held open via qmi-proxy.
  So "runs but no sats indoors" is NORMAL for this chipset + a desk, not a d2q-specific RF bug.
- **d2q stream decoded**: GPGSV lists 13 satellites IN VIEW (ephemeris — GPS time + constellation
  almanac are alive) but **every C/N0 is blank = 0 dB-Hz signal**; $PQWM1 line shows AGC active
  (measuring noise). GNSS-SV-INFO (0x25) TLV 0x10 is 1009 bytes of 32-byte records: elevation and
  azimuth present (float at +8/+12 of each record), **SNR = 0.0 on all 31** — receiver sees the
  constellation's geometry but hears nothing through the RF path.
- **Inject rejected, on purpose**: INJECT_UTC_TIME (0x38) and INJECT_POSITION (0x39) both return
  error 0x11 — Samsung's stock config doesn't accept manual assistance injection (GET_SERVER →
  no XTRA server; the engine's XTRA validity = OK via its own path).
- **Stock vendor has no GNSS RF switch config**: vendor.img mounted (/mnt/stock-vendor); the GNSS
  HAL (libgnss + android.hardware.gnss@2.1-service-qti) is just the QMI LOC client we already
  drive; no separate LNA/antenna GPIO config, no lowi daemon enabled (init.target.rc has
  `vendor.lowi` commented out). So there's **no userspace RF lever we were missing** — the WTR4905
  front-end is driven by the modem firmware itself.
- **Modem stack health**: ONLINE, crash_count 0 all session.

**Remaining question = purely acquisition sensitivity** (a first fix on this phone needs a proper
cold TTFF in open sky — typically 30 s–3 min, and our outdoor windows have been short and possibly
obstructed). gps-hold.py v2 is the tool. If a long clear-sky run STILL shows all-blank C/N0, the
last hardware check is whether the GNSS antenna itself is seated/working (phone opened before?),
but that's physical, not software.

## SESSION 5 completion — GNSS-SHARE QMI BACKEND WORKING (2026-10-30 evening)

**The GNSS userspace bridge is DONE and persistent.** gnss-share now has a `qmi` driver:
- Source: /home/usb/mnt-sda/gnss-share-src/internal/gnss/qmi_loc.go (+ 1-line hook in
  cmd/gnss-share/main.go: `case "qmi": driver = gnss.NewQmiLoc(debug)`).
- What it does: resolves the LOC service via ns-lite (port moves each boot), sets standalone
  mode via qmicli, registers events (0x21 mask), starts a session (0x22 + recurrence TLV 0x12=1),
  decodes NMEA indications (0x26 TLV 0x01) and feeds them to gnss-share's pubsub → unix socket
  /run/gnss-share.sock (1 Hz GGA/RMC/GSA/GSV/VTG + PQW status lines). Stops the session when the
  last client unsubscribes (gnss-share's own refcounting).
- **Go stdlib trap (cost the whole evening): syscall.Recvfrom returns EINVAL for AF_QIPCRTR
  source addresses** (Go's anyToSockaddr has no QRTR case) even though the kernel receive
  succeeded. All receives must go through raw SYS_RECVFROM (RawSyscall6, no addr parsing).
  Helper: qrtrRecv() in qmi_loc.go.
- **Testing trap that cost even more**: two test scripts were never chmod +x — "LAUNCHED" +
  stale /tmp files from an older run faked "0 bytes received" readings for hours. Fresh
  filenames + `chmod +x` are mandatory for phone-side test scripts.
- Config /etc/gnss-share/config: device_driver="qmi", socket /run/gnss-share.sock,
  agps_directory /var/lib/gnss-share. Unit d2q-gnss-share.service (enabled; ExecStartPre waits
  up to 12 s for the ns-lite LOC cache entry; Restart=always).
- Consumers: geoclue (via gnss-share's standard interface), gpsd (`gpsd /run/gnss-share.sock`
  style unix-stream feed needs a small adapter or gpsd's socket watch), or anything reading the
  socket. Cross-compile: `GOFLAGS=-mod=mod GOOS=linux GOARCH=arm64 CGO_ENABLED=0 go build
  -o ... ./cmd/gnss-share` from /home/usb/mnt-sda/gnss-share-src (Go on host, CGO off).
- **Unit wiring gotcha**: the modem announces services only at boot; ns-lite caches them; any
  ns-lite restart MUST bounce the modem (d2q-modem now has PartOf=d2q-qrtr-ns-lite) or the
  cache is empty and every QMI client fails with "LOC service not found".
- Fix acquisition still needs sky (indoor NMEA shows 0-signal SVs) — but the entire userspace
  path from QMI LOC to a standard NMEA socket now exists and survives reboots.

## SESSION 5 final — always-on GNSS logger (2026-10-30 evening)

- The 20-min outdoor walk was NOT captured (handoff gap: gps-hold.py 25-min window had expired, and gnss-share only streams while a client is subscribed — no file logger existed). Own it.
- Fix: d2q-gnss-log.service — persistent client of /run/gnss-share.sock appending every NMEA line to /userdata/gnss.log (reconnect loop, survives reboots). Now ANY sky time gets recorded automatically; read with tail/grep GPRMC.
- Also found two gnss-share-qmi instances running (old one survives because nohup + no unit); logger tolerates it. Cleanup next session if needed.

## SESSION 5 close — GNSS parked (2026-10-31)

- More sky time logged (1105 NMEA lines incl. outdoor), zero fixes: no GPRMC `A`, no POSREPORT, SVINFO stays header-only. Verdict stands: QMI LOC stack + bridge fully working; RF acquisition gets no signal (0 dB-Hz on all SVs, indoor AND outdoor). Parked as hardware/RF question (antenna path or front-end supply) — revisit only with the phone opened or Android-side RF state captured.
- Deliverables that stand: qmicli toolchain, ns-lite/fake-CTL/rmtfs/gnss-share units, always-on /userdata/gnss.log logger, GPS go-to-market path if RF ever resolves.

## SESSION 6 (2026-10-31) — "Android GPS works" was NETWORK location; both OSes identical

- User reported GPS working on Android 15 (Yandex Maps, indoors). Root cause of the discrepancy: **dumpsys location proves it was NOT GPS** — `gps provider: ProviderRequest[OFF]`, all Yandex HIGH_ACCURACY requests "(inactive)", every `last location=null`. Yandex was using Wi-Fi/cell (network) positioning, which works on any OS without the GNSS engine.
- LOC probe under Android (static Go tool cmd/loc-probe, adb push): engine state ON, NMEA streaming, GPRMC `,,V` (NO fix), SVINFO header-only — **byte-for-byte the same no-fix state as Debian**. Service list (vendor qrtr-lookup) also matches our NS cache 1:1 (LOC v2, servreg 66/0x42 inst 180, RFPE 41, subsystem 43 — all present on Debian too).
- CONCLUSION: not a Debian software gap. Stock Android (full Samsung RF stack) shows the identical no-fix state. Prime suspect = GNSS antenna/front-end hardware (Note10+ primary antenna path; check for drops/repairs), OR — worth one clean test — a GPS-only request in stock Android with clear sky (GPS Test app): if stock Android cannot fix outdoors either, hardware is confirmed.
- Tooling left in place: cmd/loc-probe (static Go, adb-pushable), works on both OSes (raw QRTR + QMI LOC, no deps).

## SESSION 7 (2026-10-31) — ROOT CAUSE FOUND, FIX CONFIRMED ON ANDROID

- **THE bug**: LOC_START TLV **0x10 = Fix Recurrence Type (1 = PERIODIC)** was never sent by any script/driver → every session was SINGLE-SHOT → one POSREPORT status=3 (TIMEOUT) seconds in → session dead → all outdoor walks (both OSes) ran with NO active session. The engine still streamed SV/NMEA events (engine-global), which masked the dead session.
- **Fix confirmed on Android GSI** (stock kernel+vendor): periodic session (TLV 0x10=1, 0x12=1 intermediate, 0x13=1000 ms) → POSREPORT status=0 with coordinates lat=53.6168 lon=23.8205, uncertainty converging 134m→25m over ~10 fixes. GNSS hardware is FINE; the RF "verdict" from sessions 5-6 was wrong (symptom of the single-shot bug).
- Also fixed everywhere: Register Events mask → full 0xFFFFFFFFFFFFFFFF (26-bit mask missed the SV-info list event — Debian never received SV lists).
- Deployed state: Android probe (cmd/loc-probe with correct TLVs) proven; Debian gps-hold.py v3 + gnss-share qmi_loc.go patched & rebuilt, pending deploy to Debian rootfs + outdoor verification.

## SESSION 7 final — GNSS CLOSED AS WORKING ON DEBIAN (2026-10-31)

- Outdoor walk on Debian with gps-hold.py v3 (periodic session): **449 POSREPORT status=SUCCESS**, coordinates 53.6166N/23.8198E (matches the Android fix), uncertainty 166-362m (coarse first-fix accuracy; improves with sky/assistance), up to 30 satellites heard, peak SNR 42.3 dB-Hz.
- Root cause of the entire multi-day GNSS saga: LOC_START missing the Fix Recurrence Type TLV (0x10=1=periodic) — every session was single-shot. Secondary: 26-bit event mask missed the SV-info list event (full 64-bit mask fixed it).
- Everything persistent: d2q-{stockmounts,qrtr-ns-lite,qmi-fake-ctl,rmtfs,modem,slpi,gnss-share,gnss-log}.service — the whole stack boots automatically; gnss-share serves standard NMEA on /run/gnss-share.sock for geoclue/gpsd/any consumer; /userdata/gnss.log logs continuously.
- GPS Test app (Android GSI) + loc-probe remain available for diagnostics.

## SESSION 8 (2026-10-31 night) — ACCELEROMETER: transport decoded, blocking point identified

- **SLPI exposes QRTR services**: after fixing the boot-order (SLPI booted before ns-lite → announcements dropped; d2q-slpi now PartOf=d2q-qrtr-ns-lite and SLPI restart re-announces), the SLPI appeared as QRTR **node 9** with 9 services: SEE sensor QMI **svc 0x190 (QMI_SERVICE_SSC)** node 9 port 0xc, SLPI servreg 0x42 inst 0x5a01, diagnostics 0xf/0x18/0x33 etc.
- **libssc built and talking to the SSC** (arm64 chroot: libssc 0.4.4 + libqmi main (1.39, QmiClientSsc support) built from source; Debian libqmi 1.32 lacks the SSC client. GLib<2.80 shim for g_timeout_add_seconds_once in src/compat.h; qmi-glib version check relaxed; mocking patched). Deployed: /usr/local/bin/ssccli + /opt/libssc/lib/{libssc.so.2,libqmi-glib.so.5,libprotobuf-c.so.1,libmbim-glib.so.4} on the phone; run with LD_LIBRARY_PATH=/opt/libssc/lib.
- **Where it blocks**: libssc → SSC QMI → SUID lookup for "registry" → the SLPI RESPONDS (QMI Report Large indications with the echoed "registry" datatype) but the uid list is EMPTY → "registry sensor unavailable, is hexagonrpcd running?" The SLPI sensor REGISTRY is empty: on stock, the sensor user-PDs + registry are loaded onto the SLPI via **fastRPC** (stock: libadsprpc from the sensor HAL; mainline: hexagonrpcd with the mainline fastrpc driver). Stock libs list confirms: libsns_fastRPC_util.so, libsns_registry_skel.so, libssc.so (links libqmi_cci — SEE over QMI once bootstrapped).
- **Next-session plan**: (a) RE the downstream adsprpc driver (drivers/char/adsprpc in the ShinyKernel tree) fastRPC ioctl interface + implement the PD-loading bootstrap (what libadsprpc does at HAL init: open SLPI session, load user PD /dsp/sdsp/*, push registry) — or (b) libhybris + stock sensors HAL (proven on Halium d2q, heavy setup), or (c) check whether the SLPI self-registers sensors if just given more time after boot (low odds). The transport layer is DONE — no more QRTR/boot work needed for sensors.
- Tools: ssccli (libssc CLI) on the phone; full QMI debug trace at /tmp/ssc-full.txt on the phone (2703 lines); libssc source + patches at /home/usb/mnt-sda/libssc-src (meson version relax 1.32.2, mocking girepo→glib, GLib shim).

## SESSION 8 continued — sensor PD attach achieved; strace kit staged (2026-10-31 night)

- **fastRPC init protocol decoded**: GETINFO allocates the session on the channel id passed IN the input value (0=adsp, 2=slpi); INIT{flags=FASTRPC_INIT_ATTACH_SENSORS=3} attaches to the SLPI sensors PD ("sensors_pdr_adsprpc", fl->pd=2, static kernel-handle invoke method 0 with tgid). CONFIRMED WORKING: INIT_ATTACH_SENSORS: OK via /dev/adsprpc-smd-secure (the SLPI channel is marked SECURE by default — non-secure node refuses with EACCES; secure node /dev/adsprpc-smd-secure required).
- **But the SSC registry stays empty even with the attach held** (SUID lookup "registry" → response with 0xABAB SUID-sensor uid echo per design + data_type echo, but ZERO uid entries). So the attach alone is not the bootstrap: the stock stack must ALSO push the sensor registry / load user PDs via fastRPC (libsns_registry_skel.so etc.) — OR Samsung uses a different client flow (their own libssc.so).
- Tool: /usr/local/bin/sensor-attach on the phone (source /tmp/sensor-attach.c + mnt-sda/mesa-root/root/) — hold-open daemon for the SLPI sensors session. Android capture kit staged: /tmp/android-gnss-capture.sh (restart multihal HAL under static strace + snapshot qrtr_9 ipc log + HAL fds); static strace 6.12 arm64 built in chroot (/root/strace-6.12/src/strace, 5.7 MB, --enable-mpers=no needed). PHONE IS IN FASTBOOT MODE on USB (18d1:d001) — user must boot it into Android (or Debian) to continue. When in Android: adb push strace, run the capture script, pull /data/local/tmp/cap/hal-trace.txt → the full bootstrap ioctl/registry sequence of the stock HAL.

## SESSION 8 final addendum — SSC framing fixed, SUID indications still gated (2026-10-31 night)

- **Framing fixed and validated on Android**: raw SUID lookup with (a) u16 LE size prefix inside TLV 0x01 (per libqmi SSC Control "size-prefix-format: guint16"), (b) enable_updates=true / only_default=false, now returns Control response result=SUCCESS (was error 0x13). Byte-for-byte identical to the stock HAL request (diffed from its strace: uid=0xABAB SUID-sensor, msg_id=512 fixed32, config{1:1,2:0}, SuidRequest{datatype, enable_updates=1, only_default=0}, TLV 0x10 report-type=large).
- **BUT the SUID Report-Large indications (the actual sensor list) still do not arrive** for my probe (45 s listen, 3 lookups), while the HAL received them within ~1 s. The HAL sequence: NS dump-all lookup → NS lookup svc 0x190 from socket A → registry request from socket A, wake_up_motion from socket B etc. (one socket per datatype!), indications to each socket. Remaining hypotheses: (1) an earlier syscall in the HAL sequence (the dump-all, socket ordering) flips SLPI-side state, (2) per-socket client registration, (3) a missed TLV. Next session: full ordered syscall replay from the HAL trace (11023 merged lines in /tmp/hal-trace.txt on host; reassembly script pattern exists), checking first response deltas.
- New tool: cmd/see-probe (static Go, runs on Android via adb AND Debian via ssh): NS discovery of SSC 0x190 + Samsung-framed SUID lookups + indication decode. Ready to evolve into the accelerometer driver once indications flow.

## SESSION 8 final — the SLPI registry mystery SOLVED (theory with strong evidence)

- Post-SSR state: SLPI restarted via boot_slpi/ssr → SSC QMI service announces (node 9) → but SUID lookups (even from the STOCK HAL under Android, byte-identical requests) return EMPTY uid lists. The stock HAL does NOT re-register after an SSR either — GSI sensors presumably broken post-SSR too.
- Pre-SSR (fresh GSI boot): registry lookups returned ONE real sensor uid (0x2541b4701f222275 / 0xa7007f2759215e25 pair). Something at GSI BOOT populated the SLPI registry.
- **THEORY (strong)**: the SLPI firmware reads its sensor registry from the AP via fastRPC CALLBACKS at boot (the SLPI calls the AP to read registry files — same mechanism mainline hexagonrpcd serves). Stock Android serves these callbacks at boot (sscrpcd / adsprpcd / the adsprpc driver + userspace). On Debian: no callback server → SLPI boots registry-less → no sensors → "empty registry".
- Evidence: adsprpcd + cdsprpcd + sscrpcd daemons run on stock (media/system users, /dev/adsprpc-smd holders); no /dev/sdsprpc-smd node needed; the HAL itself is pure QRTR (0 adsprpc calls in its trace — the SSR recovery trace confirms: pure QRTR with empty-registry responses).
- **NEXT SESSION PLAN**: implement the fastRPC callback/file-server on Debian for the SLPI boot: (1) capture what the SLPI requests at boot — enable adsprpc dynamic debug + watch the SLPI-glink callbacks on Debian, and/or capture the same on Android via adb (debugfs ipc + strace of adsprpcd/sscrpcd at boot); (2) replicate the minimal callback set (likely: registry file read from /vendor/dsp or /dsp/sdsp + sensor config push); (3) the SSC QRTR path (see-probe) is ready — the moment the registry populates, the accelerometer streams.
- NOTE: user must boot Android for the boot-time captures (adb), or we try the Debian-side dynamic-debug capture first (no reboot needed). Phone currently in Android.

## SESSION 8 FINAL — ACCELEROMETER ROOT CAUSE FOUND: the /persist path (2026-10-31)

- **The GSI sensor list went from 5 to 36 sensors with ONE symlink**: `ln -s /mnt/vendor/persist /persist` + sensor HAL/sscrpcd restart → "LSM6DSO Accelerometer | STMicro" and the full 36-sensor suite (accel, uncal-accel, gyro, mag...) appeared in dumpsys sensorservice. THE ROOT CAUSE of the empty SLPI sensor registry: the SLPI bootstraps its sensor registry by reading /persist/sensors/registry/... via fastRPC file-service callbacks served by sscrpcd (vendor daemon; calls libadsprpc adsp_default_listener_start; forks children that openat the files). The GSI lacked the /persist symlink → every callback open returned ENOENT → empty registry → no SSC sensors on the GSI (and the same empty-registry state on Debian).
- The registry content: /mnt/vendor/persist/sensors/registry/registry/ holds per-sensor calibration/config entries (ak0991x_0, lsm6dso_0, ...); /mnt/vendor/persist/sensors/registry/sns_reg_version = "version=1"; sns_reg_config (4189 B JSON) maps config paths → versions.
- Debian implementation plan (next session): (1) mount persist at /mnt/vendor/persist (d2q-stockmounts already mounts... persist NOT in the unit — add it), (2) create /persist symlink, (3) implement the file-server daemon = downstream-adsprpc listener: the sscrpcd trace shows the loop is CONTROL ioctls (nr 0xc, 1436 calls, many EBADRQC — the 4.14 driver only knows LATENCY/KALLOC/WAKELOCK; the Android libadsprpc uses newer control codes) + ALLOC_DMA_BUFF + forked children doing openat/read of the requested paths. Simplest viable approach: run the STOCK sscrpcd + libadsprpc.so from the vendor partition on Debian via a bionic-shim (libhybris-style) OR implement the listener ioctls directly against our driver (the EBADRQC controls suggest our driver may need the newer CONTROL handlers backported). Alternatively: check if hexagonrpcd (mainline) can be adapted to the downstream driver.
- Tools on phone: ssccli + see-probe (accelerometer SUID discovery + enable flow), sensor-attach (SLPI sensors-PD session holder), static strace. The see-probe SUID lookup framing = FINAL (u16 LE size prefix in TLV 0x01, enable_updates=1, inner-12 wrapper).

## SESSION 8 FINAL verdict — accelerometer: transport done, registry push = remaining RE (2026-10-31)

- The SSC transport is fully working from Debian: QRTR node 9 discovered, QMI framing correct, Control requests SUCCESS, registry sensor enabled, enable_updates=true subscribed.
- **The SLPI SSC firmware has ZERO registered sensors** — the SUID lookup for any datatype returns an empty uid list. This is the same on the GSI (the GSI sensor list has only 5 direct-I2C/input sensors: grip SX9360, ALS TCS3407, hall IC — NO accelerometer/gyro/mag). The "registry" is populated by the Samsung sensor stack at boot — the sensor HAL (libsnsapi/libssc) reads the config JSONs from /vendor/etc/sensors/config/ and pushes the registry entries to the SLPI via the SSC QMI service.
- **What is needed**: implement the Samsung SSC registry push protocol — the HAL reads each sensor config JSON (lsm6dso_0.json etc.), encodes the sensor attributes into protobuf (sns_reg), and sends them to the SLPI registry sensor via QMI Control messages. After the push, SUID lookups return real sensor uids and the standard enable/stream flow works (already implemented in see-probe).
- **What is already done** (usable immediately once the registry is populated): QRTR transport, SUID lookup, sensor enable (msg 513 + sample rate), measurement decode (0x401). see-probe does all of this.
- This is a well-scoped RE task: the registry push messages are visible in the HAL strace (hal-trace.txt on the phone /data/local/tmp/cap/) — the next session can decode the push format from the captured HAL traffic and implement the push in see-probe.

## SESSION 8 FINAL addendum — the definitive state (2026-10-31 late night)

- **Verbatim HAL bytes replayed on Debian** → the SLPI responds with result=FAILURE error=0x0002 (QMI_ERR_NO_MEMORY). The QRTR transport works (packet reaches the SLPI, response comes back), but the SLPI SSC service cannot allocate resources for the operation.
- **The same bytes on Android** → result=SUCCESS + Report Large indications with sensor data. The SLPI state differs between the two boots.
- **Root cause**: the SLPI SSC firmware needs its sensor registry/config loaded at boot. On stock OneUI, the config push + file-service callbacks are served by the Samsung userspace stack (sscrpcd: libadsprpc adsp_default_listener_start; adsprpcd: file server; dspservice: HIDL DSP service). On the GSI: sscrpcd runs but the /persist path was broken (fixed with the symlink → 36 sensors appeared). On Debian: sscrpcd equivalent not implemented.
- **What the next session needs**: implement the file-service/callback daemon for the SLPI on Debian. The sscrpcd trace shows: open /dev/adsprpc-smd-secure → GETINFO → INIT → CONTROL loop (1436 calls, some EBADRQC from newer control codes) + ALLOC_DMA_BUFF + forked openat/read children serving /persist paths. The libadsprpc listener protocol needs RE (from the sscrpcd-boot.txt strace on the phone, 24590 lines). Alternatively: copy the GSI bionic linker + libs to Debian and run the stock sscrpcd binary.
- **What works and persists**: GNSS (449 fixes + bridge + logger), S Pen, QMI toolchain, all infra services. The accelerometer SUID/enable/stream flow is implemented in see-probe — it just needs the SLPI in a non-degraded state.

## SESSION 8 CLOSE — accelerometer parked (2026-11-01)

- The SSC transport is fully working bidirectionally (QRTR node 9, QMI Control SUCCESS, verbatim HAL replay reaches the SLPI). The SLPI SSC firmware responds but its sensor registry has ZERO entries — no accelerometer/gyro/mag SUIDs.
- The GSI sensor list confirms: only 5 non-SSC sensors (grip SX9360, ALS TCS3407, hall IC, touch proximity, grip notifier — all direct-I2C/input devices). The SSC-based sensors (LSM6DSO accel/gyro via SLPI I2C, AK0991x mag) are absent on GSI AND Debian.
- Root cause: the SLPI SSC firmware needs its sensor registry config loaded at boot (the sns_reg_config file lists the sensor config JSONs on /vendor/etc/sensors/config/). The config load requires either the fastRPC file-service (served by sscrpcd on stock) or a QMI-level registry push from the sensor HAL. Neither is present on Debian or the GSI.
- This is a Samsung-proprietary initialization sequence (the sensor HAL reads the config JSONs, encodes the sensor attributes, and pushes them to the SLPI via SSC QMI messages). The push protocol needs RE from the HAL trace (hal-trace.txt, 12905 lines, contains the full HAL→SSC conversation including any registry initialization).
- All transport/tool work stands: see-probe (SUID lookup + enable + stream decode), sensor-attach (PD session), ssccli (libssc CLI), the QMI framing (u16 LE size prefix + inner-12-wrapper + enable_updates). When the registry populates, the accelerometer data flows immediately.

## 2026-11-01 — Registry push decode: THERE IS NO QMI PUSH
Full decode of hal-trace.txt (all 23 QRTR messages from the sensors HAL to SLPI node 9 port 0xc):
- Max payload 84 bytes. Messages are ONLY: SUID_LOOKUP (msg_id 512) for ~15 data types, GET_ATTRIBUTES (msg_id 1), ENABLE_ON_CHANGE (514). Zero config data.
- HAL's "registry" datatype lookup uses only_default_values=0; sensor lookups use 1.
- No messages to any other port/node. sscrpcd-boot.txt has NO QRTR traffic at all.
Conclusion: the SLPI registry is NOT pushed over QMI by anyone. It is PULLED by the SLPI firmware at boot through the fastRPC reverse file channel, served by sscrpcd (opens /dev/adsprpc-smd-secure).
sscrpcd file-serves (119 distinct registry files):
- /vendor/etc/sensors/sns_reg_config (master JSON: registry path -> version)
- /vendor/etc/sensors/config/*.json (lsm6dso_0.json, msmnile_lsm6dso_0_{0,1,6}.json, ak991x, lps22hh, tmd4907, pah_8011, sns_amd, ...)
- /mnt/vendor/persist/sensors/registry/registry/* (lsm6dso_0.accel[.config], lsm6dso_0_platform.accel.fac_cal[.bias,.corr_mat], placement, orient, temp..., power.gpio.*, sns_tilt, sns_smd, sns_rmd, ...)
- also opens /persist/sensors/registry/registry and /sys/class/sensors/ssc_core/ssc_mode
All those files live on partitions Debian already mounts (vendor=sda25, persist=sda5 at /mnt/vendor/persist + /persist symlink). So the ONLY missing piece on Debian is a running sscrpcd answering the fastRPC file callbacks at SLPI boot.
Plan (unchanged, now confirmed as THE path): finish bionic-sscrpcd (/opt/bionic on phone; needs 4 libs pulled from GSI /system/lib64: libhidlbase.so, libutils.so, libion.so, libvndksupport.so), run with LD_LIBRARY_PATH=/opt/bionic/system/lib64:/opt/bionic/vendor/lib64:/vendor/lib64, restart SLPI, see-probe SUID lookup -> enable -> stream.

## 2026-11-01 — ACCELEROMETER WORKING ON DEBIAN (cold-boot persistent)
The bionic-sscrpcd path closed the sensor stack:
- Pulled libhidlbase/libutils/libion/libvndksupport/libdl_android from GSI /system/lib64 → /opt/bionic/system/lib64. sscrpcd now runs under /opt/bionic/system/linker64 with LD_LIBRARY_PATH=/opt/bionic/system/lib64:/opt/bionic/vendor/lib64:/vendor/lib64.
- d2q-sscrpcd.service (Before=d2q-slpi, After/Requires=d2q-stockmounts) + d2q-slpi-ssr.service (oneshot SSR kick After=sscrpcd+slpi+ns-lite, then restarts d2q-sensor-attach) — the cold-boot race: SLPI boots at ~5s, before sscrpcd attaches the rpmsg channel; the SSR at stack-up re-boots the SLPI against the live file server AND re-announces services to ns-lite.
- PROOF: strace of sscrpcd during SSR shows the SLPI pulling all 119 registry files (lsm6dso_0.accel, fac_cal.*, ...) + /vendor/etc/sensors/sns_reg_config + config JSONs + /vendor/lib64/libssc_default_listener.so — byte-identical pattern to the working GSI.
- uid-probe (cmd/uid-probe in gnss-share-src): replays GET_ATTRIBUTES to real sensor UIDs extracted from hal-trace (SUID discovery still unanswered, but direct UID addressing works — SUCCESS + attribute reports). lsm6dso accel = UID 0x1fbb6afc01727ea6 / 0x9a418d19f8d7ba44 (attribute report strings: "lsm6dso", "STMicro", datatype "accel", sns_accel.proto). ENABLE_CONTINUOUS (msg 513, sample_rate 50.0f) → Report Large indications with fixed32 x/y/z: phone flat → z ≈ -9.81 m/s² (gravity), live noise on x/y. ~2 Hz effective rate (sample-rate field needs tuning).
- Traps hit: pkill -f self-matching the ssh session cmdline (3x — use scripts); `echo b > /proc/sysrq-trigger` discards unsynced page cache (sync first!); NM profile d2q-rndis was MAC-pinned to a stale address (now pinned to ifname enp0s20f0u4); scp raced the reboot leaving a 0-byte unit file (verify md5 after transfer).
- Remaining: rate tuning, gyro/mag UIDs, userspace bridge (iio/evdev feed) for Debian apps.

## 2026-11-01 — CAMERA: enumeration + open WORKING on Debian
- The Samsung camera stack is 32-BIT ONLY (com.samsung.chi.override.so — the usecase definitions that create logical cameras — ships only in /vendor/lib/hw; the 64-bit camera.qcom.so can never enumerate). Shim must be armv7 bionic.
- 32-bit bionic runtime staged from the STOCK firmware dump (/mnt/stock-sys/system/apex/com.android.runtime/{bin/linker,lib/bionic/*} + system/lib/*) → phone /opt/bionic32 (60 libs incl. deliberately-broken libqti-perfd-client.so — kills the PERF_LOCK_OPEN_CAMERA hang: chi override dlopens it, fails, skips perf).
- cam-shim32 (host src /home/usb/mnt-sda/camera-recon/cam-shim32.c): hand-rolled _start → __libc_init; HMI layout measured from RELR slots: 32-bit hw_module_t = 128 bytes, get_number_of_cameras @+0x80, get_camera_info @+0x84 (64-bit: hw_module_t = 248 bytes, fn ptrs @0xf8+).
- Run: LD_LIBRARY_PATH=/opt/bionic32:/vendor/lib /opt/bionic32/linker /usr/local/bin/cam-shim32 → **number_of_cameras=5 (3 back + 2 front), open camera 0 rc=0 (dev API 3.5), initialize rc=0, 185 static metadata entries readable.** imx516 ToF detected (640x480+320x240). Sensors probed over CCI, EEPROMs read.
- Traps found: (1) get_number_of_cameras=0 root cause = CamX 64-bit scans /vendor/lib64/camera for sensormodule .bin but Samsung ships them in /vendor/lib/camera (32-bit path) → use 32-bit stack; (2) /dev/video0 (cam-req-mgr) is a singleton — stale processes hold it (fuser + kill); (3) fake logd: python AF_UNIX dgram listener on /dev/socket/logdw captures ALL CamX logs on Debian (phone /tmp/fake-logd.py); (4) ioctl-spy.so LD_PRELOAD dumps VIDIOC_CAM_CONTROL (0xC01856C0) op codes — op 0x101=CAM_QUERY_CAP; (5) the open hang = HIDL getService spin in libhidlbase for vendor.qti.hardware.perf@2.2::IPerf (perfd absent on Debian) — fixed by shadowing libqti-perfd-client.so with a text file (dlopen fails → chi override skips perf locks).
- NEXT: milestone 2b — configure_streams (RAW10) + ion buffers + process_capture_request → first frame.

## 2026-11-01 — CAMERA session 2: configure_streams works, capture blocked on stream ABI
Working on Debian via cam-shim32 (32-bit bionic, /opt/bionic32 from stock dump + LD_LIBRARY_PATH=/opt/bionic32:/vendor/lib):
- enumerate=5, open cam0 rc=0, initialize rc=0, configure_streams rc=0 (preview 1440x1080 YUV + capture 4032x3024 YUV_420_888).
- ICP fw: must be in /lib/firmware (kernel fw loader never searches /vendor/firmware) — copied CAMERA_ICP.elf; "FW download done successfully".
- Property area: deployed GSI /dev/__properties__ snapshot to Debian /dev/__properties__ (host: camera-recon/props/props.tar). Killed the SystemProperties::Update spin... BUT the hang during open was actually PERF LOCK (libqti-perfd-client dlopen → shadowed with a text file in /opt/bionic32). Property area still needed for other things; deploy persists via systemd (TODO: unit).
- Hand-crafted QTI private_handle_t (ver=12, fds=2, ints=23, magic 'gmsm' 0x6d736d67, flags USES_ION 0x80000000, size/offset/format/width/height at data[7..12]) passes the gralloc validator.
- BLOCKER: process_capture_request → CamX worker SIGSEGV in CamX::L17::configure_streams+6836 region — reads stream array [r9][r6*4] then [stream+0x1c]=data_space. Suspect: Samsung's camera3_stream/_configuration layout differs (vendor extension), or stream handling needs the buffers pre-registered. camxhal3 logs "frame_number 1, num_output_buffers 0" (the 0 = HAL-internal after import, NOT a struct offset — sentinel-fill proved it).
- NEXT: (a) decode exact Samsung camera3_stream/apture_request layout via Ghidra on configure_streams+process_capture_request, (b) consider driving CSL directly (cam-req-mgr ioctls) bypassing CamX HAL entirely — the kernel ABI is known (VIDIOC_CAM_CONTROL op codes in cam_defs.h).
- Files: host /home/usb/mnt-sda/camera-recon/{cam-shim32.c, gtest.c, ioctl-spy.c, thermal-stub.c, camera32.qcom.so, com.samsung.chi.override.so, gralloc.msmnile.so, libgralloccore.so, bionic32/, props/}, phone /usr/local/bin/{cam-shim32, fake-logd.py, relogd.sh}, /var/log/camx.log.

## 2026-11-01 — CAMERA session 3: request ABI fixed, pipeline streamConfig is the wall
User's structured SyncManager investigation resolved the crash: **the crash was CamX::L23::process_capture_request, NOT SyncManager** (my earlier vaddr symbolization mixed file-offset vs vaddr). Real cause: **Samsung's camera3_capture_request has num_output_buffers at offset 12 and output_buffers at offset 16 (SWAPPED vs AOSP)** — verified from HAL disassembly (validation reads [r11+16]=output_buffers array, [r11+12]=count; the BLOB check reads output_buffers[i].stream->format == 0x21). cam_sync itself is fine (video1 opens+subscribes). After the struct fix: no crash; process_capture_request returns EINVAL because device->session (priv+68) is NULL.
Root cause of null session: configure_streams builds the usecase but nodes log **"CreateBufferManagers: invalid type streamConfig"** (IFE0/IPE0/FDHw0) + "FinalizeBufferProperties optimalHeight is 0" → pipeline incomplete → session null. This is CamX usecase/property-driven.
Also confirmed: (a) CDSP boots via `echo 1 > /sys/class/subsys/subsys_cdsp/device/fw_load` (on-demand PIL, same as kgsl zap); (b) /dev/cdsprpc-smd correctly absent (CDSP = /dev/adsprpc-smd chan 3, verified vs OnePlus SM8150 vendor source); (c) property area from GSI (Android 15) is REJECTED by our Android-12 bionic libc (opens property_info, fstat, closes without reading → all property reads empty on Debian). CamX reads persist.vendor.camera.* (sync_en, multiframe_en, mfnr_en, fastaec.enable...) for usecase/feature selection — empty on Debian.
NEXT: either (a) get a working property area/service so chi override picks the right usecase, or (b) trace the exact streamConfig type the nodes need (GSI trace has the working config). The request pipeline: ConfigureStreams→chi usecase XML→node streamConfig (vc/dt/type) → CreateBufferManagers. On GSI, ZSL pipelines built (pipelineId 0/1, 3648/4032).
