# d2q 4K60 Investigation — Full RE Report
**Date: 2026-11-03/04 · phone: SM-N9750 (d2q) · stock Android 12 with KernelSU root**

## Status: 4K30 WORKS, 4K60 BLOCKED BY SAMSUNG'S UNIHAL LAYER

All other camera features work: telephoto (camera 52), ultra-wide (50), OIS, variable aperture, FHD30, 4K30, 5-module color photos.

---

## 1. Architecture discovery (correcting earlier assumptions)

### The stock camera stack is ENTIRELY 32-BIT

```
vendor.samsung.hardware.camera.provider@4.0-service  ← ELF 32-bit ARM (12KB thin wrapper)
  └─ SehCamDevSession (HIDL device@5.0)
       └─ camera.unihal.default.so (2.8KB HAL module wrapper)
            └─ unihal_main@1.3.so (878KB — the REAL Unihal logic)
                 └─ camera.qcom.so (32-bit, 6.6MB CamX HAL)
                      └─ com.samsung.chi.override.so (32-bit, 2.6MB Samsung CHI)
```

The 64-bit copies in `/vendor/lib64/hw/` exist but are **never loaded** by the stock camera system. The 32-bit and 64-bit CHI have identical usecase sets (verified by strings diff).

### Unihal layer (the missing piece from all previous analysis)

Samsung's Unihal sits between the HIDL session and the CamX HAL. Even in "bypass mode" (which the 4K60 video session uses), Unihal:
1. Processes session_parameters through ParameterManager
2. Stores the stream config through ScenarioManager  
3. Passes the stream config **completely unchanged** to the vendor HAL
4. BUT: replaces the camera3_device ops table at open time

The call chain (fully decompiled):
```
uni_open_camera_hardware → CameraHalManager::OpenCamera → MCM::OpenCamera
  → CameraDevice::OpenCamera → vendor HAL open
  → REPLACE device.common.close (offset 0x3c) with Unihal close
  → REPLACE device.ops (offset 0x40) with Unihal wrapper ops table

CameraHalUnit::ConfigureStreams → MCM::ConfigureStreams
  → CameraDevice::ConfigureStreams → vendor HAL ops->configure_streams
  (stream config pointer passes through UNCHANGED in bypass mode)
```

---

## 2. The FPS mechanism (fully decoded)

### Sensor mode selection chain

```
getSensorDimension(camId, w, h, &outW, &outH):
    fps = *(chi .bss @ 0x2772EC)   // "usecase FPS" global
    if (fps == 0) fps = 30          // default
    DesiredSensorMode.fps = fps
    DesiredSensorMode.modeIndex = -1  // auto
    FindBestSensorMode(camId, &desired)
        → iterates sensor modes, scores by resolution + FPS
        → if modeIndex >= 0: direct selection (skip scoring)
```

### Sensor mode table for camera 40 (SAK2L4 wide)

```
Mode 0: 4032x3024 @ 30fps
Mode 1: 4032x3024 @ 24fps
Mode 2: 4032x2268 @ 30fps
Mode 3: 4032x2268 @ 24fps
Mode 4: 4032x1908 @ 30fps
Mode 5: 4032x1908 @ 24fps
Mode 6: 3024x3024 @ 30fps
Mode 7: 3024x3024 @ 24fps
Mode 8: 4032x2268 @ 60fps  ← THE 60FPS MODE
... (up to mode 19, includes 240/480/960 fps HFR modes)
```

### What we proved about the FPS global

- Poking 60 into `chi .bss @ 0x2772EC` works (old value was 0 → default 30)
- The poke is read by `getSensorDimension` as a **hint** for FindBestSensorMode
- But the **usecase's pipeline descriptor** determines the actual sensor mode
- The 3-stream config matches a 30fps usecase → sensor runs at 30fps regardless of the poke

---

## 3. What we tried (comprehensive matrix)

### 3-stream video config (WORKS at 30fps)

```
preview:    0x22, 1920x1080, usage 0x102/0x20900, maxbuf 4
record:     0x22, 3840x2160, usage 0x30000, maxbuf 8
previewCb:  0x23, 1920x1080, usage 0x20003, maxbuf 4
```

Result: configure rc=0, frames delivered at 30fps.

### With ALL 60fps triggers added (still 30fps)

```
+ FPS global poked to 60 (chi .bss @ 0x2772EC)
+ sensor_mode_index = 8 (in request AND session params)
+ AE_TARGET_FPS_RANGE [60,60] (in request AND session params)
+ sensor.frameDuration = 16666666 (in request AND session params)
+ captureIntent = VIDEO_RECORD
+ recordingMaxFps = 60 (vendor tag)
+ targetFPS = 60 (vendor tag)
+ recordingHint = 1
+ captureMode = 3 (video)
+ request cloned (84 entries copied + overrides)
+ session_parameters with all above
```

Result: ALL accepted (every tag rc=0, configure rc=0), but **frameDuration = 33333 us (30fps)**. The sensor mode is fixed by the usecase.

### Exact 5-stream Unihal ground truth (FAILS)

From the stock Unihal log (the REAL table the CHI receives):
```
stream[0]: 1920x1080,  format 0x22, usage 0x900,   ds 0x0,      option 0x1
stream[1]: 3840x2160,  format 0x22, usage 0x10000, ds 0x104,    option 0x41
stream[2]: 1920x1080,  format 0x23, usage 0x3,     ds 0x8C20000, option 0x1
stream[3]: 4032x2268,  format 0x21, usage 0x3,     ds 0x8C20000, option 0x2
stream[4]: 4032x2268,  format 0x23, usage 0x3,     ds 0x8C20000, option 0x2
```

Result: **ENODEV (-19)** on stock AND Debian. Errors:
```
chxadvancedcamerausecase.cpp:4080: Invalid Format: 0x23
chxadvancedcamerausecase.cpp:4080: Invalid Format: 0x21
DefaultMatchingUsecase() Failed to match usecase. pSelectedUsecase is NULL
SelectUsecaseConfig() No proper usecase selected
```

### Camera IDs tried with 5-stream replica

Camera 0, 20, 40, 41, 52 — all ENODEV (-19) except 41, 50 (EINVAL -22).

---

## 4. Samsung stream struct (decoded from binary)

### 32-bit camera3_stream layout (Samsung extension)

```c
struct camera3_stream {           // 32-bit
    int stream_type;              // 0x00
    uint32_t width;               // 0x04
    uint32_t height;              // 0x08
    int format;                   // 0x0c
    uint32_t usage;               // 0x10
    uint32_t max_buffers;         // 0x14
    void* priv;                   // 0x18
    int32_t data_space;           // 0x1c
    int rotation;                 // 0x20
    const char* physical_id;      // 0x24
    int32_t streamOption;         // 0x28 ← Samsung! (AOSP: dynamic_range_profile)
    int32_t samsung_stream_id;    // 0x2c
};
// sizeof = 0x30
```

### 64-bit hw_module_t layout (discovered empirically)

```c
struct hw_module_t {              // 64-bit (aarch64)
    uint32_t tag;                 // 0x00
    uint16_t module_api_version;  // 0x04
    uint16_t hal_api_version;     // 0x06
    const char* id;               // 0x08
    const char* name;             // 0x10
    const char* author;           // 0x18
    hw_module_methods_t* methods; // 0x20
    void* dso;                    // 0x28
    uint64_t reserved[26];        // 0x30..0xF7
};                                // total 0xF8

struct camera_module {            // 64-bit
    hw_module_t common;           // 0x00..0xF7
    get_number_of_cameras         // 0xF8
    get_camera_info               // 0x100
    set_callbacks                 // 0x108
    get_vendor_tag_ops            // 0x110
    open_legacy                   // 0x118
    set_torch_mode                // 0x120
    init                          // 0x128
};
```

(32-bit: hw_module_t = 0x80, camera_module fns at 0x80-0x98 — standard.)

### streamOption values (from stock)

| Value | Meaning |
|---|---|
| 0x1 | preview |
| 0x2 | photo/still capture |
| 0x41 | video record (4K60) — bit 0x40 = 60fps capable |
| 0x4 | thumbnail |
| 0x320002 | secondary photo (ultrawide) |

---

## 5. Unihal decompilation (complete)

### CameraStream::GetScenarioType(camera3_stream*)

Reads streamOption at offset 0x28 (confirms our offset):
- format 0x23 (YUV): option bit 6 (0x40) → scenario 14 (60fps capable)
- option & 0xf == 1 → scenario 4 (preview)
- option & 0xf == 2 + 0x20000 bits → scenario 7
- option & 0xf == 4 → scenario 12 (thumbnail)
- physical_camera_id parsing for multicam scenarios

### CameraHalUnit::ConfigureStreams (bypass path)

```
1. ParameterManager::SetSessionParameters(pm, config->session_parameters)
2. ScenarioManager::SetFwkStreamConfiguration(sm, config)
3. CheckEnableCondition() → disabled (foundScenario=false for VIDEO)
4. → bypass: pcVar8 = param_1 (original config, UNCHANGED)
5. MultiCameraManager::ConfigureStreams(mcm, cameraId, pcVar8)
6. → CameraDevice::ConfigureStreams(cd, cameraId, pcVar8)
7. → vendor_ops->configure_streams(device, pcVar8)
```

### uni_open_camera_hardware

```
1. CameraHalManager::OpenCamera → MCM::OpenCamera → CameraDevice::OpenCamera
   (calls vendor HAL's open)
2. device->common.close (offset 0x3c) = unihal_close
3. Read original ops table (offset 0x40)
4. Create new ops table, for each of 11 function pointers:
   if original is NULL → new = 0
   else → new = Unihal wrapper function
5. device->ops = new_ops_table
```

### VendorHalLoader::AdjustPublicModuleOps

Just mirrors NULL function pointers from vendor HAL to wrapper module. Cosmetic.

---

## 6. The remaining suspects (for next session)

### A. Stream struct SIZE difference (PRIME SUSPECT)

The Camera3Stream wrapper (from device@5.0-impl) is 0x68+ bytes per stream. Our camera3_stream_t is only 0x30. The streams are in a contiguous array. The CHI might read wrapper fields at offsets 0x30–0x67 of each stream, reading into the NEXT stream's data (which is valid memory but wrong context).

**Test**: pad each stream to 0x68 bytes (add 56 zero bytes after each 0x30-byte struct).

### B. CameraHalUnit::Create (open-time state)

Called when a camera is opened through the Unihal wrapper. May call `initialize()` with Unihal callbacks (not framework callbacks), and do additional HAL state setup.

**Test**: decompile CameraHalUnit::Create from unihal_main@1.3.so.

### C. Unihal callback registration

The Unihal's initialize wrapper might register different callbacks with the vendor HAL, affecting CamX's internal callback handling and pipeline construction.

### D. Unihal static-init crash (blocks direct testing)

The unihal_main@1.3.so has a static-init order fiasco: VendorHalLoader's constructor calls MultiCameraManager::GetVendorHalModule() before the MCM singleton is constructed. This crashes our shim when trying to load the unihal wrapper.

**Fix options**: 
- Patch the binary (2-byte change to skip the null check)
- Or: pre-construct the MultiCameraManager before unihal_main loads
- Or: call the HIDL path that properly initializes it

---

## 7. Tools and environment

### Ghidra setup (working)

```
JDK: /home/usb/mnt-sda/jdk21 (portable, downloaded from Adoptium)
Ghidra: /home/usb/mnt-sda/ghidra/ghidra_11.3.2_PUBLIC/support/analyzeHeadless
Projects: /home/usb/mnt-sda/ghidra-proj/
  - chiRE (32-bit com.samsung.chi.override.so)
  - chiRE64 (64-bit com.samsung.chi.override.so)
  - devImpl (vendor.samsung.hardware.camera.device@5.0-impl.so)
  - unihal (unihal_main@1.3.so)
Scripts: camera-recon/ghidra_scripts/*.py
```

### Ghidra notes

- Ghidra REBASES ELF at +0x10000 — subtract 0x10000 from displayed addresses to get ELF vaddrs
- Jython scripts need `# -*- coding: utf-8 -*-` header (no non-ASCII)
- RTTI survives in chi .so — real class::method names recovered
- To find string references: use getReferencesTo with 2-hop (string → pool → code)

### Key offsets (32-bit CHI: com.samsung.chi.override.so)

```
FPS global:           ELF vaddr 0x2772EC (Ghidra 0x2872EC)
getSensorDimension:   0x8a8a4 (Ghidra 0x9a8a4)
FindBestSensorMode:   0x7b860 (Ghidra 0x8b860)
GetMatchingUsecase:   0x8779c (Ghidra 0x9779c)
```

### Key offsets (64-bit camera.qcom.so)

```
hw_module_t size:     0xF8
camera_module fns:    0xF8 - 0x128
HMI symbol:           vaddr 0x5b4900, 368 bytes
streamOption offset:  0x30 (in 64-bit camera3_stream)
```

### cam-shim64 (built, tested)

- Built with `clang --target=aarch64-linux-android -nostdlib -fuse-ld=lld`
- Links against bionic64/ (libc, libdl, libcamera_metadata from stock /system/lib64)
- Struct layouts fixed for aarch64 (hw_module_t = 0xF8)
- Crashes at sensor-module enumeration (fileCount: 0) — 64-bit CamX can't find .bin files
- NOT needed — stock is 32-bit anyway

---

## 8. Next session roadmap

1. **Try stream padding**: extend each stream slot from 0x30 to 0x68 bytes
2. **Decompile CameraHalUnit::Create** from unihal_main@1.3.so
3. **Patch unihal static-init crash** and load the full Unihal stack
4. **Or**: intercept the vendor HAL's configure_streams with an LD_PRELOAD-like wrapper to dump the EXACT stream bytes the stock provider passes
5. **Or**: accept 4K30 and move to other features

---

## 9. All working features (unchanged)

| Feature | Status | Notes |
|---|---|---|
| Telephoto (camera 52) | ✅ | S5K3M5, 2x optical zoom |
| Ultra-wide (camera 50) | ✅ | S5K3P9 |
| Wide (camera 40/0) | ✅ | SAK2L4 main |
| Front (camera 1/41) | ✅ | IMX374 |
| ToF (camera 4/80) | enumerated | rejects YUV, needs depth format |
| FHD30 video | ✅ | mp4 delivered |
| 4K30 video | ✅ | 3840x2160 frames decoded |
| Variable aperture | ✅ | manual f/1.5/f/1.8/f/2.4 + auto |
| OIS | ✅ | mode 1, kernel CAM-OIS confirmed |
| 5-module photos | ✅ | color NV12 decoded |
