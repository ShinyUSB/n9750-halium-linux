# d2q audio bring-up — investigation notes (2026-11-04)
**Device: SM-N9750 (d2q) · Debian 12 · kernel 4.14.190-ShinyKernel**

Goal: stereo audio (cs35l41 ×2 I2C amps, i2c-7 0x40/0x41) through the Qualcomm ADSP (Q6)
audio path. This is the "big lift" the memory warned about — here is the decoded chain and
what was done.

## Hardware / driver inventory (all present and bound on Debian)

- cs35l41 ×2 on i2c-7 (7-0040, 7-0041), driver `cs35l41` bound
- Machine driver `samsung-q6audio-adaptation` bound (`soc:samsung,q6audio-adaptation`)
- `audio_apr` bound (`soc:qcom,msm-audio-apr`), APR v2 compiled (CONFIG_MSM_QDSP6_APRV2_RPMSG)
- SLIMbus NGD ctrl bound (171c0000.slim), codec = WCD934x "tavil"/pahu (headphone codec)
- No sound card: registration is gated on the whole APR/q6 stack being up.

## The boot chain (decoded from techpack/audio + drivers/soc/qcom)

1. **ADSP boot** → `subsystem_get("adsp")` (framework path!) → subsys-restart runs the
   framework powerup → **SUBSYS_AFTER_POWERUP notifications** fire.
2. `glink_probe.c` registers the ADSP glink edges ONLY on AFTER_POWERUP → without that
   notification: no glink edge, no `apr_audio_svc` rpmsg channel, no ADSP QRTR node.
3. `apr_tal_rpmsg` connects on the `apr_audio_svc` channel.
4. `apr_adsp_up()` (apr.c) — prints "Q6 is Up", creates the q6 child platform devices
   (q6core/q6afe/q6asm/q6adm) via `add_chld_dev_work`. Fired by the audio_notifier chain.
5. q6 DAI components register → machine driver's deferred `devm_snd_soc_register_card`
   retries → **card appears**.
6. audio_notifier (techpack/audio/dsp/audio_notifier.c) ADSP domain uses the **PDR**
   service (servreg over QRTR): `get_service_location("avs/audio")` (service-locator.c,
   kernel QMI) → servreg **locator service 0x40** (a userspace pd-mapper on Android!) →
   domain list ("msm/adsp/audio_pd", instance 0x4a) → per-domain state via service-notifier
   (kernel looks up the ADSP's own servreg **0x42 inst 0x4a01**, node 5) → SERVICE_UP →
   `apr_adsp_up`.
7. slimbus NGD (slim-msm-ngd.c `ngd_dom_init`) uses the SAME locator query
   ("appsngd0"/"avs/audio") — codec enumeration depends on it; its SSR fallback has NO
   AFTER_POWERUP handler, so PDR is mandatory for the WCD934x too.

## Root causes found (both fixed)

**RC1 — fw_load bypasses the framework.** `/sys/class/subsys/subsys_adsp/device/fw_load`
(d2q's own on-demand PIL interface, image 246+) calls `subsys_powerup`→`pil_boot` directly:
ADSP runs but NO subsystem notification → glink edges never register → APR/QRTR dead
(observed: ADSP booted for an hour, zero glink/apr/qrtr activity).
**Fix:** boot via the adsp-loader: `echo 1 > /sys/kernel/boot_adsp/boot` (adsp-loader.c
`adsp_boot_store` → `subsystem_get("adsp")` — the full framework path). Verified: glink
edge registers, fastrpc + `apr_audio_svc` channels open, ADSP appears on QRTR as **node 5**
(servreg `0x42 inst 0x4a01`).
**Persisted as `d2q-adsp.service`** (After=d2q-qrtr-ns-lite).

**RC2 — the kernel servreg locator lookup is one-shot and races userspace.**
`init_service_locator` (service-locator.c) sends ONE `NEW_LOOKUP` for service 0x40 at
probe time (~1.5 s after boot) and waits 300 s. The QRTR name service (ns-lite, userspace)
binds the ctrl port ~5-6 s after boot — the lookup is sent BEFORE ns-lite exists and is
lost forever; after the 300 s wait `service_timedout` latches and **poisons the locator
for the whole boot** (audio PDR + slimbus NGD + fastrpc audio-PD notifier all dead).
The audio_notifier's own fallback to SSR_ADSP fires only on PDR failure and its state
stays DOWN until the NEXT ADSP restart — which is why the SSR restart trick worked:
`echo 1 > /sys/kernel/boot_adsp/ssr` (subsystem_restart_dev) → AFTER_POWERUP → the
SSR-fallback clients get SERVICE_UP → **"apr_adsp_up: Q6 is Up" observed** even without
PDR. But the slimbus NGD has no SSR fallback → codec still missing → kernel patch needed.
**Fix (kernel patch, image 302):** service-locator.c `init_service_locator` now re-sends
the lookup every 10 s (60 tries) instead of latching after one 300 s wait.

## Userspace pieces added on Debian

- **pd-mapper-lite** (`/usr/local/bin/pdmapper_lite.py`, unit `d2q-pdmapper.service`):
  binds QRTR port 0x50, serves QMI_SERVREG_LOC_GET_DOMAIN_LIST (0x0021) for "avs/audio":
  domain "msm/adsp/audio_pd", instance_id **0x4a** (= the ADSP servreg instance on the bus).
  Encoding per drivers/soc/qcom/qmi_encdec.c: TLV=[type u8][len u16 LE][val], strings
  [u8 strlen][bytes], array count [u16 LE], everything LE. Verified byte-exact round trip.
- **ns-lite STATIC** += `{0x40: (1, 0x50)}` — kernel lookups for the locator now answered.
- **ns-lite push**: NEW_SERVER announcements are now PUSHED to clients that registered a
  lookup for that service (kernel qmi sockets accept unsolicited NEW_SERVER —
  qmi_recv_new_server does not filter). This fixes the same one-shot race for the
  service-notifier's 0x42 lookup (ADSP boots after the lookup was sent).

## Boot image 302

`camera-recon/bootimg-301/boot-302-audio-pdr.img` (sha 8e2ff41bfd2964dd…):
kernel = 301 + service-locator retry patch; ramdisk/cmdline identical to the running
boot-301-tof-fw-v2 (cs40l25a firmware preserved); header-diff vs v2 = id field only
(pack-d2q.sh check passed). Flash: `heimdall flash --BOOT boot-302-audio-pdr.img`.

## What to expect after flashing 302 (test procedure)

1. `d2q-adsp` boots the ADSP ~10 s into the boot; dmesg shows
   "apr_tal_rpmsg ... Channel[apr_audio_svc] state[Up]" and "apr_adsp_up: Q6 is Up".
2. ns-lite journal shows "pushed svc 0x42..." and pd-mapper-lite serves the domain list.
3. `/proc/asound/cards` should show the Samsung card (machine "sound-tavil" name).
4. Mixer: cs35l41 paths via alsamixer/amixer; test `speaker-test -c2` or `aplay` a wav.
5. If the card registers but a tone is silent: check AFE port config + cs35l41 amp
   enable/power routing (next stage).

## Traps learned

- fw_load = framework-less PIL (no notifications) — use boot_adsp/boot for anything that
  needs glink/APR/notifier plumbing.
- service-locator latches one timeout per boot (`service_timedout` static) — patched.
- ns-lite lookups were one-shot; kernel qmi expects push — added push support.
- `killall <script.py>` does NOT kill python scripts (name = python3) — use
  `kill $(pgrep -f name)` or kill by pid.
- struct.unpack("<BBHH", data[:7]) — the format is 6 bytes; slicing 7 bytes raises
  struct.error("unpack requires a buffer of 6 bytes") — a too-large buffer is ALSO an error.
- python AF_QIPCRTR explicit low-port bind (< QRTR_MIN_EPH_SOCKET) needs root; as root it
  works and holds the exact port (verified getsockname (1, 80)).
- ssh links drop on compound commands with systemctl restarts (known trap) — short commands.

---

# SESSION 2 RESULTS (2026-11-04 late) — STEREO SOUND WORKING ✅

**The d2q plays real stereo audio through both cs35l41 amps on Debian, fully organic at boot.**
User-confirmed audible. Card registers automatically ~9.5 s after boot.

## The remaining bugs after image 303 (all userspace, all fixed)

1. **pd-mapper-lite QMI encoding (3 bugs, each found by kernel-side evidence):**
   - kernel QMI header is **7 bytes**: `[type u8][txn u16 LE][msg_id u16 LE][msg_len u16 LE]`
     (qmi.h: txn_id is u16!) — my 6-byte assumption made the msgid check never match
   - TLV header is `[type u8][len u16 LE]` = 3 bytes — my `[type][pad][len]` made the kernel
     read len=0x0400 → garbage decode → (in 302) the SLIMBUS_DEBUG panic → BOOTLOOP
   - the domain_list TLV type is **0x12** (not 0x01) per the resp ei table, and its element
     count is a **u8** (`.elem_size = sizeof(uint8_t)` in the DATA_LEN entry), value =
     `[u8 count][u8 strlen][name][u32 instance][u8 valid][u32 data]`
   - optional elements: ONE TLV each, NO flag byte (the decoder infers valid from presence)
   - verified end-to-end with a python simulation of qmi_decode + live round trips
2. **ns-lite lookup instance filter**: the kernel's servreg lookup carries the wanted
   instance (version|pd_id<<8 = 0x4a01 for the ADSP). ns-lite replied with ALL 0x42
   servers (modem 0xb401, SLPI 0x5a01, ADSP 0x4a01); the kernel's service-notifier
   overwrote its destination per callback → the listener reg went to the WRONG subsystem's
   servreg → QMI error 0x2d → -121. Fixed: instance-matched lookups AND instance-matched
   pushes.
3. **ADSP boot delay**: the ADSP's servreg announcement fired ~4.7 s, BEFORE ns-lite bound
   the ctrl port → never cached. Fixed: d2q-adsp.unit sleeps 4 s (After=ns-lite), so the
   announcement lands in the cache and the push delivers it to the kernel's pending lookup.
4. **The panic() removed**: service-locator.c under CONFIG_SLIMBUS_DEBUG panicked
   ("Failed to get process domains") on any locator failure → the 302 bootloop. Now falls
   back like the non-debug path.

## Working boot sequence (all automatic, verified on cold boot)

```
~1.5 s  kernel audio drivers probe; locator lookups registered (service-locator retry
        patch re-sends every 10 s until answered)
~4.9 s  ns-lite up (static 0x40 → pd-mapper port 0x50); pd-mapper-lite serving
~8.8 s  d2q-adsp boots the ADSP via boot_adsp/boot (framework path, 4 s delay);
        glink edges register; apr_audio_svc channel up; ADSP servreg announces
        (0x42 inst 0x4a01) → ns-lite caches + PUSHES to the kernel's pending lookup
~8.9 s  kernel service-notifier registers the listener with the ADSP servreg
        (msm/adsp/audio_pd) → state 0x1fffffff = UP → audio_notifier UP →
        "apr_adsp_up: Q6 is Up" → q6 child devices probe
~9.5 s  "Sound card sm8150-tavil-snd-card registered"
~20 s   d2q-audio-init sets the speaker path
```

## Mixer recipe (cs35l41 stereo on SEC_TDM_RX_0)

```
amixer -c0 cset name="SEC_TDM_RX_0 Audio Mixer MultiMedia1" 1
amixer -c0 cset name="Left AMP Enable Switch" 1
amixer -c0 cset name="Right AMP Enable Switch" 1
amixer -c0 cset name="Left AMP Mute" 1        # enum: 0='Muted', 1='Unmuted' (BACKWARDS!)
amixer -c0 cset name="Right AMP Mute" 1
amixer -c0 cset name="Left AMP PCM Gain" 20   # 0-20, 20 = max
amixer -c0 cset name="Right AMP PCM Gain" 20
aplay -D hw:0,0 file.wav
```

Trap: Samsung's cs35l41 "AMP Mute" enum is inverted (amp_mute=1 means UNMUTED, the
driver code comments even say so). Setting 0 muted the amps — the "nothing audible"
that cost one debugging round.

## Units / files on the phone

- `/usr/local/bin/pdmapper_lite.py` + `d2q-pdmapper.service` (port 0x50, Restart=always)
- `/usr/local/bin/qrtr-ns-lite.py` — STATIC 0x40 entry + instance-filtered lookups/pushes
- `/etc/systemd/system/d2q-adsp.service` — sleep 4 + boot_adsp/boot
- `/usr/local/bin/d2q-audio-init.sh` + `d2q-audio-init.service`
- kernel: image 303 (`boot-303-audio-pdr2.img`, sha 38d647e2…) = service-locator retry +
  panic removal; header-diff vs the running image = id field only

## Not done yet (next steps)

- PulseAudio/PipeWire layer for desktop use (raw ALSA works)
- The cs40l25a-side "Values stored" big-data check at each stream open is normal
- Reader-mode NFC regression test still pending (from the NFC session)

---

# SESSION 3 (2026-11-04 night): NFC regression + ToF investigation

## NFC regression: PASS ✅
Reader mode (`/root/pn547_ce2 r`) after all config changes: detected the same ID card,
`RF_INTF_ACTIVATED ... ba 95 6d 21` (T2T, UID ba:95:6d:21 — identical to the original
detection). Both NFC modes (reader + CE) fully intact.

## ToF (IMX516): status + findings (NOT working yet — zeros at all resolutions)

**What works:**
- Camera 4 (public id) = IMX516 ToF; the shim's `t` mode now takes dims/fmt args:
  `cam-shim32 4 t [w] [h] [fmt]` (nreq is still the LAST arg before them... call form:
  `cam-shim32 4 t 1280 3840 8` — the 5th numeric arg is FMT currently, nreq hardcoded by
  arg count — see run_tof: argv = [bin] [camid] [mode] [w] [h] [fmt]; nreq comes from the
  trailing number semantics in shim_main).
- 1280x3840 Y16 configures rc=0, frames deliver at the full size (9,830,400 B/frame),
  plus the internal depth-engine result streams (frame ids 255+, outputs=0).
- The ToF EEPROM calibration is present (rear/rear2 dual-tilt cal logged at open).
- Sensor streams: "[dynamic_mipi] IMX516 mode : 0", "[TOF_FREQ_DBG] tof_freq : 100",
  "TOF BB enabled" (the BB GPIO is driven high at CAM_START_DEV — note: stock d2q kernel
  has CONFIG_SEC_D1Q_PROJECT=y which COMPILES THIS OUT; our kernel has it unset so the BB
  path runs — the stock kernel handles the BB differently!).

**What doesn't:** ALL frames zero at 640x480 (computed depth) AND 1280x3840 (raw phase).

**Findings this session:**
1. **VCSEL (LD) control found**: `/sys/devices/virtual/camera/rear/rear_tof_ld_onoff` —
   writes IMX516 I2C 0x2134/0x2135 (0x00,0x00 = LD on; 0xAA,0xAA = off). Requires an OPEN
   sensor session (g_s_ctrl_tof is set on CAM_ACQUIRE_DEV, cleared/NULL otherwise → write
   fails "Operation not permitted" when idle). Writing 1 mid-session succeeded — frames
   STILL zero.
2. **Sensor mode mismatch**: the HAL picked "IMX516 mode : 0" for our 1280x3840 Y16
   stream. The Samsung sensor-mode table (vendor tag 0x80290000) lists modes 2/3 for
   1280x3840 — mode 0 is likely a small/idle register set → the MIPI delivers no real
   phase data. The mode choice comes from CamX's stream→mode matching; a Y16 stream may
   need the RAW16 ToF format or a Samsung depth request param to select the right mode.
3. **The dump path fix**: cam-shim32 dumped video frames to /data/local/tmp (Android
   path) — now /tmp/vidNN.raw on Debian.

**Conclusion**: the ToF capture chain is plumbed end-to-end (configure → stream →
buffers) but the sensor delivers no pixels without the FULL stock depth-session setup:
the right sensor mode + the laser sequencing + possibly Samsung depth request params.
This is exactly the situation 4K60 was in. **The proven path forward = a stock
ground-truth session**: boot the KernelSU stock image, run the OneUI depth feature
("3D scanner" / portrait-depth), capture logcat + camx.log, extract: (a) the stream
table, (b) the sensor mode the stock stack picks, (c) the LD/laser sysfs/I2C sequence,
(d) the Samsung request params (rawParam). The EEPROM dual-tilt cal + rear_tof sysfs
infra are all present on Debian — once the session setup is replicated, depth can be
computed host-side (4-phase lock-in: depth ∝ atan2 of the phase quadrants, unwrapped
with the 100 MHz modulation frequency noted in the kernel log).

---

# SESSION 4 (2026-11-05, stock A12 + KernelSU root) — ToF ground truth hunt

**Bottom line: the ToF is wired end-to-end but NOTHING on the CHN stock firmware streams it —
Live Focus is dual-camera bokeh, AR Drawing is Google's motion stereo.**

## What the stock session established (definitive)

1. **Live Focus (portrait) = `SecDualBokehNode` = DUAL-CAMERA bokeh**, NOT ToF. Logcat:
   `E V1/SecDualBokehNode: mTofCalibration is fail on onInitialized, mAvailableTofData : false`.
   The kernel never acquires the ToF sensor in this mode (g_s_ctrl_tof stays NULL).
2. **AR Drawing (com.samsung.android.ardrawing + ARCore) = Google's motion_stereo**
   (`vr/perception/depth/projects/motion_stereo/spherical_rectifier.cc` — RET_CHECK failures:
   kUnrectifiedPinhole vs kUnrectifiedOriginal) — photometric dual-frame disparity from the
   COLOR camera, NOT the ToF.
3. **No Quick Measure / 3D Scanner on CHN** (N9750ZSU6HZE1 = CHN variant; those are intl).
4. **The ToF depth pipeline EXISTS in the stock vendor**: CHI usecase `SecCamTOF`
   (vendor/qcom/proprietary/chi-cdk/vendor/chioverride/samsung/chxseccamtofusecase.cpp),
   `com.samsung.node.uniplugin_depth.so` (reads /sys/class/camera/rear/rear_tof_ae_value!),
   `com.qti.node.depth.so`, `libdepthmapwrapper.so`, `SecCamTofRawBufferManager`,
   `YuvUniToTofSession`, `FeatureTof Session: m_pDepth16Stream / m_pDepthConfidenceStream /
   m_pTofPreviewStream`. Stream strings: "Depth16 Stream" (Y16 = TARGET_BUFFER_Y16),
   "DepthConfidence Stream", "TofPreview Stream". Stock CamX: "Found depth sensor(imx516),
   [0] WxH = 640x480, [1] WxH = 320x240" — those are the depth-plugin OUTPUT sizes.
5. **The SecCamTOF usecase never matches our raw Y16 streams** (no "Matching Usecase id =
   SecCamTOF" in the shim's logcat). The trigger is the CHI usecase matcher in
   com.samsung.chi.override.so — the exact stream table + rawParam that selects it needs
   RE (Ghidra, the 4K60 method).
6. **cam-shim32 runs on stock** (LD_LIBRARY_PATH=/vendor/lib:/system/lib; provider must be
   stopped: `setprop ctl.stop sec-camera-provider-4-0`; restored with ctl.start). Our
   640x480 Y16 session on stock: same all-zero frames (the depth plugin doesn't auto-engage
   from a bare HMI session). Dumps now work on stock (the /tmp → /data/local/tmp fallback).
7. **The factory camera app exists** (com.sec.factory.camera, FactoryCameraActivity +
   CameraTestActivity + CameraFirmwareActivity) but crashed/permission-gated when launched
   (splash shown then destroyed) — a lower-effort possible ToF trigger if ever reachable.

## The concrete next step
Ghidra RE of the override lib: find SecCamTOF's stream table signature + the rawParam
(shootingmode / app mode marker) that selects it. After that our shim drives the ToF the
way the depth pipeline expects, and the host-side depth compute is straightforward (4-phase
lock-in, 100 MHz modulation, dual-tilt cal already in the EEPROM).

Ground truth logs: host /tmp/nfcwork/{tof_gt_logcat.txt,tof_gt_dumpsys.txt,ar_gt_dumpsys.txt}.

---

# SESSION 5 (2026-11-05): SecCamTOF usecase RE + trigger hunt

## Ghidra RE of com.samsung.chi.override.so (chiTOF project)

Decompiled the SecCamTOF path (the full override lib, 32-bit ARM, 2.5 MB — RTTI mostly stripped
but log strings survive). The ToF depth stack in the CHI override:

- `SecCamTOFUsecase` (chxseccamtofusecase.cpp) — the usecase class
- `FeatureTOF` (chxsecfeaturetof.cpp) — the feature (ActivateTof/DeactivateTof)
- `SecCamTofRawBufferManager` / `SecCamTofDepth16BufferManager` — the buffer managers
- `YuvUniToTofSession` + `OfflineYuvUniToTofSessionProcessResult` — the YUV→ToF session
- The session streams: **Depth16 stream (Y16 0x20363159)** + **DepthConfidence stream** +
  **TofPreview stream** + the raw ToF input

## The usecase's accepted streams (from SecCamTOFUsecase::Create)

Per-stream checks (32-bit Samsung stream: format@3, usage@4, ext@7):
- **format 0x22 (IMPLEMENTATION_DEFINED) + usage & 0x100** (CAMERA_INPUT) → the camera input
  (stored at this+0x26ac0)
- **format 0x21 + stream[7] == 0x1000** → stored at this+0x26ad4 (the raw ToF phase stream)
- **format 0x20363159 (Y16) + stream[7] != 0x1000** → the Depth16 output
- format 0x20 (RAW_SENSOR) → another path (al_zoom_control / am_optical_zoom_control names —
  interesting: the dual-cam zoom control!)
- num_streams validation: 1 stream OK if the raw ToF stream present; 2 streams OK too

## Empirical probes on stock (cam-shim32 's' mode = configure-only)

| config | rc | note |
|---|---|---|
| 0x20363159 (Y16) 640x480 alone | -19 | no match |
| 0x23 (YUV) 1280x3840 / 640x1920 | -22 | format rejected |
| **0x21 (BLOB) 640x480 usage 0x1000** | **0** | **ACCEPTED — but "Invalid Format: 0x21" from the advanced usecase, no SecCamTOF match** |
| 0x21 640x480 + 0x21 bigger / 2-stream combos | -19/-22 | rejected |

So the SecCamTOF usecase needs a MULTI-stream (likely multicam) config — single-camera
configs don't reach it. The trigger conditions need either the full matcher RE or the real
app session (a Samsung depth app on intl firmware).

## The honest ToF status

The hardware is fully functional (factory *#0*# test passes; sensor streams; laser sysfs
controllable). The DEPTH engine (SecCamTOF + uniplugin_depth + CDSP) needs a multicam
SecCamTOF session — our shim's single-camera configs don't select it. Remaining options:
1. Ghidra RE of the full SecCamTOF match predicate (the UsecaseSelector's condition table
   — the usecase descriptor lists stream patterns; chxadvancedcamerausecase.cpp:4080
   "Invalid Format 0x21" suggests the ADVANCED usecase path is what's actually running).
2. Multicam session attempt: open camera 0 (wide) + camera 4 (ToF) with the
   SecCamTOF-exact stream table (once RE'd).
3. The DEPTH output (640x480 Y16) then comes from the depth engine automatically.

Factory camera app exists (com.sec.factory.camera, FactoryCameraActivity) but is
permission-gated — couldn't launch it. *#0*# "ToF test" = the sensor register read test
(passes → hardware healthy).

---

# SESSION 6 (2026-11-05): the SecCamTOF match predicate REVERSED + flashed back to Debian

## The trigger condition (from GetMatchingUsecase disassembly at 0x888d8, chiTOF project)

The usecase selector scans every output stream and matches the TOF usecase when it finds:
```
stream_type == OUTPUT(0)  AND
(format == 0x20363159 (Y16)  OR  format == 0x21)  AND
usage == 0x1000
```
(the arm disasm: `ldr r4,[r5,#0xc]` = format, cmp Y16 / cmpne 0x21; `ldr r5,[r5,#0x1c]` = usage;
`cmp r5,#0x1000; movweq r1,#0x1`.)

Then a second check on the LogicalCameraInfo field +0x80 (values 1-3 vs >3) picks the branch.
The SecCamTOF session is a MULTICAM session: it needs the ToF stream (0x21/0x1000) PLUS the
color camera — a single-camera config hits the predicate but then takes the wrong branch (the
advanced usecase logs "Invalid Format: 0x21").

## The verified findings (this session, all reproduced on stock)

- **The `0x21 640x480 usage 0x1000` stream config IS ACCEPTED** (configure rc=0) — it satisfies
  the first predicate stage; the subsequent camera-info check sends it to the advanced usecase.
- **The multicam session is the answer**: camera 0 (wide color) + camera 4 (ToF raw) — the
  sec TDM out + a raw ToF input stream.
- The full stack: SecCamTOF usecase → SecCamTofRawBufferManager + SecCamTofDepth16BufferManager
  → YuvUniToTofSession (the offline depth) + uniplugin_depth.so (reads rear_tof_ae_value) +
  libdepthmapwrapper.so.

## What remains (the concrete next experiment)

Build the multicam session in cam-shim32 (mode 'w' exists): camera 0 preview (0x22) + camera 4
ToF stream (0x21 1280x3840 usage 0x1000) + the Samsung rawParam — that should select SecCamTOF.
The depth16 output then flows from the depth engine automatically. All files:
host /home/usb/mnt-sda/ghidra-proj/chiTOF (the analyzed lib) + camera-recon/chi_tof_*.txt.

Debian flashed back (image 303): sound card registers organically at boot; NFC CE + reader work.

---

# SESSION 7 (2026-11-05): SecCamTOF trigger SOLVED — the usecase engages, depth pipeline initializes

## The trigger (REVERSED + REPRODUCED ON DEBIAN)

From GetMatchingUsecase disasm (chiTOF) + empirical: a stream with
**format 0x21 (BLOB) + data_space 0x1000** on camera 4 → **SecCamTOFUsecase engages**.
cam-shim32 t-mode now: `cam-shim32 4 t 640 480 0x80000021` (fmt bit31 = set
TS.data_space=0x1000). The 's' mode gained data_space support (same bit31 convention).

## What runs once the usecase engages (all verified in camx.log)

- `camxchinodeUniDepth.cpp: SetUniInitParamInfo: shootingMode = 1`
- `UniPluginUnit.cpp: UniPluginInitialize: [TOF_PROC] Sensor Name is IMX516.`
- **`TOFProc CONFIG_INFO: mode_uid: 0xBC13 tof_type: 5 isFactoryBin: 0`** — the depth DSP
- `compute_set_cal: modelingFile not found → using in-code modeling, version_modelParm 190710, num 20`
- A **TOFPointCloudOut1** output stream is created by the usecase
- the sensor driver job "imx516" registered
- frames flow (results/buffers back at ~4.5 fps — the ToF is slow)

## The remaining failure (all zeros)

- `SubmitChiRequest Fail` (the usecase's realtime CHI request) — the internal ToF input
  request fails
- `CreateBufferManagers() Node::TOFPointCloudOut1_ChiNodeWrapper0 invalid type streamConfig`
  — the point-cloud output stream config
- The sensor stays in "IMX516 mode : 0" (not the phase mode) → raw frames all-zero

## The final step

The SecCamTOF usecase's Initialize creates the full session streams. Read
SecCamTOFUsecase::Initialize (chiTOF, 000cc320 area + the usecase's CreateCameraLog/
Initialize functions) to extract the EXACT internal stream table (the sensor stream's
format/size + the depth/confidence/pointcloud outputs). The usecase class members:
m_pDepth16Stream, m_pDepthConfidenceStream, m_pTofPreviewStream + TOFPointCloudOut1.

---

# SESSION 7 final note

The SecCamTOF usecase's own Initialize (which builds the internal raw-phase + depth16 +
confidence + point-cloud streams) sits under the base Usecase hierarchy — the class's method
names are stripped; the trigger is SOLVED (0x21 + data_space 0x1000 on camera 4), the depth
pipeline initializes, and the remaining gap is the internal stream table the usecase expects
(the "SubmitChiRequest Fail" + the TOFPointCloudOut1 invalid-type errors). To close it:
decompile SecCamTOFUsecase's Initialize (its log tag differs from chxseccamtofusecase.cpp —
hunt by "SecCamTOFUsecase: " strings) — the stream dims/formats/usages there give the exact
config for a working session.
