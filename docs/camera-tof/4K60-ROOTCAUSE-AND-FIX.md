# d2q 4K60 — ROOT CAUSE FOUND & WORKING
**Date: 2026-11-03/04 · phone: SM-N9750 (d2q) · stock Android 12, KernelSU root · device serial RF8M82H3NFM**

## STATUS: 4K60 ACHIEVED through the vendor stack — confirmed on stock AND Debian

The 5-stream 4K60 session now **configures (rc=0), selects the stock `SecCamVideo` usecase,
requests sensor mode 4032×2268@60, and delivers 3840×2160 frames at ~62 fps (16.08 ms median
inter-frame)** with valid image content. This is the same path the stock camera app uses.

**Debian confirmation (2026-11-04):** the same shim runs on Debian 12 (kernel 4.14.190-ShinyKernel).
The 3-stream video path (`cam-shim32 0 H 8`) reports `sensor.frameDuration = 16666 µs` in every
capture result (55/55 uniform) — direct sensor-level proof of 60 fps — with `exposureTime ≈ 9991 µs`
(< 16.6 ms, consistent), and a real 4K frame written. The 5-stream path (mode `9`) configures rc=0
and delivers 4K frames. The only addition needed vs stock: the rawParam injection (below), which the
shim now also performs in `run_session2` when a 60 fps session is requested.

The blocker was never the stream table, the ABI, the metadata tags, or the FPS global. It was a
**Samsung client-identity channel** that the stock framework drives and our shim (and any
non-Samsung client) did not.

---

## A. Root-cause ranking

### 1. (CONFIRMED ROOT CAUSE) Missing `samsungcamera=true` session identity → CHI caps at "Regular Session Max FPS 30"

**The stock framework's `CameraService` package-checks the calling app.** For
`com.sec.android.app.camera` it logs `CameraService: isSamsungCamera: this package is Samsung app: true`
and builds a **rawParam string**:

```
first-entrance=false;samsungcamera=true;factorytest=false;shootingmode=3;recording-fps=60;
sw-vdis=false;video-beautyface=false;vtmode=0;operation_mode=none;ssm_shot_mode=0;
recording_dr_mode=sdr;sw-super_vdis=false;stream_type=0;
```

This string is pushed to the HAL through a Samsung HIDL extension and lands in
`CamX::set_parameters`, which sets HwEnvironment/CHI globals. With it, the CHI logs
**`Regular Session Max FPS 60`** and selects `SecCamVideo`. Without it, the CHI logs
**`Regular Session Max FPS 30`**, treats the client as `THIRD_PARTY_PREVIEW`, and:
- 3-stream config → matches AdvancedCameraUseCase (snapshot pipeline) → sensor locked at 30 fps
  regardless of AE fps range / frameDuration / sensor_mode_index / the FPS global poke.
- 5-stream config → usecase match fails entirely → ENODEV.

Evidence: stock logcat `isSamsungCamera … true`, rawParam `shootingmode=3;recording-fps=60`;
our run without rawParam → `Regular Session Max FPS 30` + ENODEV; our run **with** rawParam →
`Regular Session Max FPS 60` + `SecCamVideo` + 62 fps delivery.

Supporting/contradicting: every earlier "60 fps trigger" (AE range [60,60], sensor.frameDuration,
sensor_mode_index=8, recordingMaxFps, targetFPS, captureMode, the CHI .bss FPS global poke) was
*accepted* but *ignored* for the sensor rate — because the usecase/fps-cap decision happens
upstream of all of them, gated on the client identity.

### 2. (CONFIRMED, second-order) 5-stream ENODEV = AdvancedCameraUseCase format rejection on the wrong usecase

Not a memory/ABI fault. The 5 streams reached the CHI intact; the selector (non-Samsung path)
routed to `chxadvancedcamerausecase.cpp`, whose `Initialize` rejected the YUV/BLOB snapshot pair
(`Invalid Format: 0x23 / 0x21`), `DefaultMatchingUsecase()` returned NULL, `SelectUsecaseConfig()`
failed, and `CamX::HALDevice::ConfigureStreams` returned **-ENODEV**. This is a logical
"no usecase matched" result, not a device fault. See §D.

### 3. (RULED OUT) Stream-struct size / ABI mismatch

Our shim used 0x30-byte `camera3_stream_t`; the real vendor object is 0x68 (below). Yet the
3-stream path worked at 30 fps with 0x30 structs, and fixing the size to 0x68 did **not** by
itself change the frame rate or fix the 5-stream ENODEV. The CHI usecase matcher only reads
fields ≤ 0x28 (incl. `streamOption` @ 0x28), all inside both layouts. ABI correctness matters for
correctness/safety, but it was not the 30 fps cause.

---

## B. ABI findings (all binary-verified on the 32-bit vendor stack)

Compiled an armv7 layout probe (`abi_probe.c`, static asserts, compiles clean) against the
vendor-era `camera3.h` in `/home/usb/n9750-halium/hardware/libhardware/include`, and cross-checked
against the 32-bit binaries:

**`camera3_stream_t` (Samsung 32-bit) = 0x68 bytes** — AOSP base fields then Samsung extension:

| off | field | source |
|---|---|---|
| 0x00 | stream_type | AOSP |
| 0x04 | width | AOSP |
| 0x08 | height | AOSP |
| 0x0c | format | AOSP |
| 0x10 | usage (u32) | AOSP |
| 0x14 | max_buffers | AOSP |
| 0x18 | priv | AOSP |
| 0x1c | data_space | AOSP |
| 0x20 | rotation | AOSP |
| 0x24 | physical_camera_id | AOSP |
| 0x28 | **streamOption** | Samsung (AOSP `reserved[0]`) |
| 0x2c..0x67 | Samsung ext | **zeroed** by `convertFromHidl` |
| (0x68) | `mId` | `Camera3Stream` subclass member, not part of the HAL struct |

Proof chain:
- 32-bit `device@5.0-impl` `convertFromHidl(SehStream*, Camera3Stream*)` (vaddr 0x249db)
  writes `mId` at **dst+0x68**, and `streamOption` at **dst+0x28** (from `SehStream+0x40`);
  zeroes dst 0x14/0x18/0x24 and 0x2c–0x67.
- `Camera3Stream : public camera3_stream` (AOSP `device/3.2/default/include/convert.h`) →
  `mId` directly after the base ⇒ base = 0x68.
- `unihal_main@1.3.so` `unihal::CameraStream::CameraStream` does
  `__aeabi_memcpy4(this+0x10, stream, 0x68)` — Unihal copies the full 0x68-byte struct.
- **AOSP 32-bit base** (this header) = **0x40** (`reserved[6]` @ 0x28..0x3f). Samsung reuses
  `reserved[0]` (0x28) for `streamOption` and extends to 0x68.

**`camera3_stream_configuration_t` = 0x10**: num_streams@0, streams@4 (`camera3_stream_t**`
pointer array — verified in CHI: `*(int*)(param_2+4) + i*4`), operation_mode@8 (a Samsung vendor
opmode 0xf001 exists), session_parameters@0xc. No AOSP input_width/height.

**`camera3_capture_request_t` (32-bit)**: frame_number@0, settings@4, input_buffer@8,
num_output_buffers@0xc, output_buffers@0x10 (+ physcam fields). Matches our working shim.

**Pointer relationship (the user's P1 question)**: `streams[]` is a true **array of pointers** to
independently valid 0x68 objects. `GetScenarioType()` receives the **native `camera3_stream_t*`**
(`unihal::CameraStream` stores the framework pointer at +0x8 and calls `GetScenarioType(param_1)`
on it). No flat-array-to-pointer-array cast exists in the real path. **Where streamOption is
written**: `convertFromHidl` (from the HIDL `SehStream`+0x40), which the Samsung *framework*
`SessionConfig`/`Camera3-Status` set from the app's request (`streamOption: 65` for the 4K60
record surface). **Where it's read**: CHI `GetScenarioType`/`GetMatchingUsecase`
(bit 0x40 ⇒ "60fps-capable", bit 0x20, bit 0x2), `chxseccamvideousecase`, etc.

`streamOption` values (stock): 0x1 preview, 0x41 record(4K60), 0x2 photo, 0x4 thumbnail.

---

## C. Pipeline / usecase findings

**Why the matching algorithm chose the 30 fps descriptor (before the fix):**

`UsecaseSelector::GetMatchingUsecase` (chi vaddr 0x8779c) consumes:
- CHI globals: `isSamsungCamera` ("use samsug camera"), factory-binary (`ro.build.PDA`=="FA"),
  **shooting mode**, operation mode, etc.;
- per-stream fields ≤ 0x28: `stream_type`, `format`, `usage` (incl. bit 0x10000 = VIDEO_ENCODER),
  `data_space`, **`streamOption`** (bits 0x2 / 0x20 / 0x40).

For our non-Samsung-marked session it took the third-party branch → `THIRD_PARTY_PREVIEW(3)` →
the 30 fps AdvancedCamera usecase and a 30 fps sensor mode. The CHI even logged
`InitializeOverrideSession: Regular Session Max FPS 30`.

**What the stock 4K60 session selects:** `SecCamVideo` (`chxseccamvideousecase.cpp`, log tag
`[CHI_VIDEO]`), pipeline nodes `VideoPreview_IFE0/IPE0/FDHw0/LRME0`, sensor mode requested =
**4032×2268 @ 60** (sensor mode index 8 in the SAK2L4 table), delivered frameDuration
**16666666 ns**.

**Where 60 fps is gated:** the AOSP-visible `android.scaler.availableMinFrameDurations` is
**33333333 ns for every 3840×2160 format** (0x22/0x23/0x21), and `aeAvailableTargetFpsRanges`
tops out at [30,30]. The 60 fps capability is exposed **only** via Samsung vendor metadata:
`samsung.android.scaler.availableVideoConfigurations` advertises `[3840 2160 60 60]`, and
`org.quic.camera2.customhfrfps.info.CustomHFRFpsTable` lists `3840×2160 @ 60`. So a third-party
Camera2 client structurally cannot reach 4K60; the Samsung app reaches it via the private
`0x7fa30c06` record format + `streamOption 0x41` + the `samsungcamera=true` rawParam.

**4K60 descriptor existence:** yes — `SecCamVideo` is the descriptor; it is selected once the
session is identified as Samsung video (`shootingmode=3` + `recording-fps=60`).

---

## D. ENODEV findings (5-stream config)

Exact originating chain (stock and Debian identical):

```
CamX::configure_streams (camera.qcom.so 0x1a9a44)
  → CheckValidStreamConfig (0x1b42c0)          OK
  → CHI InitializeOverrideSession / GetMatchingUsecase
      → (non-Samsung path) AdvancedCameraUseCase
        → chxadvancedcamerausecase.cpp:4080 Initialize: "Invalid Format: 0x23" / "0x21"
        → chxusecaseutils.cpp:1036 DefaultMatchingUsecase: "Failed to match usecase. NULL"
        → chxadvancedcamerausecase.cpp:4346 SelectUsecaseConfig: "No proper usecase selected"
  → camxhaldevice.cpp:1055 ConfigureStreams: "CHI Module failed to configure streams"
  → returns -ENODEV (-19)
```

The error is generated at **usecase-selection time**, before any pipeline/sensor/ISP setup, and is
propagated unchanged up to `configure_streams`. It is a "no matching usecase" (`NULL` selection)
surfaced as `-ENODEV`, not a hardware/transport failure. After the return the device is cleanly
closed; no persistent error state.

---

## E. The decisive experiment (what actually moved the needle)

Single change that flipped 4K60: **inject the Samsung rawParam through the vendor ops extension.**

`SehCameraDeviceSession::sehSetParameters(hidl_string)` (device@5.0-impl vaddr 0x1beb8) does:
```
dev = *(this+0x1c);  ops = *(dev+0x40);  fn = *(ops+0x28);  if (fn) fn(dev, str.c_str());
```
`ops+0x28` = index 10 = AOSP `reserved[2]` → **`CamX::set_parameters`** (camera.qcom.so 0x1ad108),
which parses `samsungcamera`, `shootingmode`, `recording-fps`, `operation_mode`, `factorytest`,
`recording_dr_mode` and writes the HwEnvironment/CHI identity+FPS globals.

Our shim (talking directly to camera.qcom.so, bypassing the framework) now calls:
```c
typedef int (*set_params_fn)(const camera3_device_t*, const char*);
((set_params_fn)ops->reserved[2])(c3, "first-entrance=false;samsungcamera=true;factorytest=false;"
    "shootingmode=3;recording-fps=60;sw-vdis=false;video-beautyface=false;vtmode=0;"
    "operation_mode=none;ssm_shot_mode=0;recording_dr_mode=sdr;sw-super_vdis=false;stream_type=0;");
```
Runtime: `ops=0xe87ecd64 … [10]=set_params 0xe8347a08`, `rc=0`.

**Result:** 5-stream rc=0, `SecCamVideo`, `Regular Session Max FPS 60`, sensor mode
4032×2268@60, and 150 video frames at a 16.08 ms median interval (**~62 fps**), real image data.

Two measurement traps eliminated along the way:
- The per-frame request must target only the 3 real-time streams (preview/record/previewCb); the
  photo streams (BLOB/YUV) are configured but only requested for still-capture (like stock).
  Requesting all 5 every frame starved on the 1-buffer photo streams and mimicked ~30 fps.
- The SecCamVideo path returns **NULL** result metadata (`r[1]=0`), so frameDuration can't be read
  from callbacks there; buffer-arrival timing is the reliable probe (once not starved).

---

## F. Implementation proposal (minimal patch to enable 4K60)

The vendor stack needs no binary patch. Enable 4K60 by satisfying the client-identity gate and the
usecase inputs at configure time:

1. **Identify as the Samsung video client** — call the vendor ops extension slot
   (`ops+0x28` / `ops->reserved[2]`) once after `open`/`initialize` with
   `samsungcamera=true;shootingmode=3;recording-fps=60;…`. *(exact target: `CamX::set_parameters`
   via ops index 10; exact change: the rawParam string; effect: CHI marks the session Samsung,
   lifts "Regular Session Max FPS" to 60, and selects `SecCamVideo`).*
2. **Streams**: provide the stock 5-stream set with `streamOption`@0x28 (record = 0x41) and the
   video stream as `0x22 + usage 0x10000` (VIDEO_ENCODER) + data_space 0x104. Use 0x68-byte
   stream objects (zeroed extension) for ABI-correctness.
3. **Session/request metadata**: `samsung.android.control.shootingMode (0x805b000d)=3`,
   `recordingMaxFps (0x805b0050)=60`, `android.control.aeTargetFpsRange=[60,60]`,
   `captureIntent=VIDEO_RECORD`. (Present in both the session params and the request.)
4. **Per-frame requests** target only the real-time streams.

Possible regressions: marking the session "Samsung" routes through `SecCamVideo`, which expects
the Samsung stream topology — a plain 3rd-party-style config may now hit the same "no usecase"
ENODEV; gate the rawParam to only the configs we support. None of the existing working features
(telephoto/ultrawide/OIS/aperture/photos/FHD30/4K30) are affected — they don't set the rawParam.

Rollback: stop calling the ops extension (the default state is the pre-fix behavior). No persistent
state is written; the flags live in the process's HwEnvironment only.

### Residual / not-yet-nailed-down
- Whether `recording-fps=60` alone (vs the full string) is sufficient — the full stock string was
  used; a minimal-key sweep can isolate it.
- Whether the 3-stream (H) path can also reach 60 fps once given the rawParam — untested; the
  5-stream `SecCamVideo` path is the stock-accurate one.
- The framework-side trigger that flips the FPS global the CHI reads for `FindBestSensorMode`
  (the poke to chi .bss 0x2772EC was already accepted; with the rawParam the session requests 60
  natively).

---

## Evidence index
- Stock 4K60 ground truth: `stock-4k60-logcat.txt`, `stock-4k60-dumpsys.txt`
  (frameDuration 16666666; SecCamVideo; rawParam `shootingmode=3;recording-fps=60`).
- Fresh debug-4 captures: `stock-4k60-logcat-dbg4d.txt`, `shim5-sp.txt`, `v60t_logcat.txt`.
- Working run: `/data/local/tmp/v60f.log` (phone), `v60f.log` analysis — 62 fps median.
- ABI: `abi_probe.c`/`abi_probe.o` (armv7), `vendor32/vendor.samsung.hardware.camera.device@5.0-impl.so`.
- RE: `qcom_rawparam.txt` (set_parameters/configure_streams/CheckValidStreamConfig),
  `chi_ucmatch.txt`, `chi_ssparam2.txt`, Ghidra project `qcom32`.
- Shim (working 4K60): `cam-shim32.c` → `cam-shim32` (mode `9`).
