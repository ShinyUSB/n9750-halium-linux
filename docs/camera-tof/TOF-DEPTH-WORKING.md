# ToF (IMX516) depth WORKING on Debian — 2026-11-05

The IMX516 rear ToF sensor produces **real per-pixel 3D depth** on Debian via the
Samsung SecCamTOF CHI usecase + the TOFProc engine. Validated: 640×480 pointcloud,
~99.97% valid points, temporally stable, scene-coherent depth map.

## One-command capture

```
tof-capture [nframes]          # phone-side wrapper, frames -> /tmp/vidNN.raw
```

Equivalent manual form:

```
LD_LIBRARY_PATH=/opt/tofpatch:/vendor/lib:/opt/bionic32 \
  /opt/bionic32/linker /usr/local/bin/cam-shim32 4 t 640 480 80000021 [nframes]
```

## Output format (the pointcloud blob, /tmp/vidNN.raw)

- 36-byte header (u32[0] = 0x4b000 = 307200 = point count).
- then 640×480 × 16-byte little-endian `{x, y, z, intensity}` float32.
- `z` = depth in metres. Valid points have 0.1 < z < 20 (and |x|,|y| sane).
- Buffer size CamX wants: 36 + 640*480*16 = 4915236 (+12 pad => 4915248).

## The trigger (from session 7)

Camera id **4** (= logical 5, CAM_REAR_TOF, IMX516). A single framework stream:

- format **0x21 (BLOB)**, **data_space 0x1000**, size 640x480 (the initial size is
  irrelevant — the usecase mutates it, see below).

This selects the `SecCamTOF` usecase (UsecaseTOFRaw16 descriptor) and runs the
depth pipeline: UniDepth node -> TOFProc engine (iu456 datapath) + calibration.

## THE TWO FIXES (both required; without either you get all-zero frames)

### Fix A — buffer size (request-chain blocker, was the "SubmitChiRequest Fail")

`SecCamTOFUsecase::Create` (com.samsung.chi.override.so, 0x1cb944) **mutates our
0x21+ds0x1000 stream in place** during configure_streams():

```
our_stream->height = 1;                                   // BLOB convention
our_stream->width  = (capsW>>1)*(capsH>>3)*16 + 36;       // = 4915236 (1280x3840 caps)
```

i.e. our stream becomes the **point-cloud output container**. CamX's
`TOFPointCloudOut1` pipeline then imports OUR framework buffers for that port and
`CSLMapNativeBuffer` -> `OsUtils::MemMap` mmaps **4915248** bytes
(640*480*16 + 48). The shim used to allocate 640*480*2 = 614400 bytes, so the
dma_buf mmap failed **EINVAL** (614400 page-aligns below the request; 4915236
page-rounds above it, which is why stock works). That single failure cascaded:
`CSLHwAddBuffer failed` -> `CSLMapNativeBuffer failed` -> `SubmitChiRequest Fail`
-> the whole realtime request chain errored -> **the sensor never actually
streamed** (all-zero frames, "IMX516 mode : 0" was a symptom, not the cause).

Fix (cam-shim32 run_tof): after configure_streams, **re-read the mutated
TS.width/TS.height** and size the ion buffers to the mutated blob size,
page-rounded (>= 4915248). Handle's w/h fields set to the mutated dims too.

### Fix B — TOFProc engine raw-input width (one-instruction plugin patch)

With the chain fixed, the engine rejected its raw-phase input at init:

```
TOFProc.Size of inputRaw is not supported!!! 9830472x8
```

The usecase presents the sensor's 1280x3840 raw16 frame as a byte-blob:
width = 2 * 4915236 = 9830472 (= 1280*3840*2 + 72). The plugin's IMX516 config
(`TOFProcPlugin::set_plugin` case 0x28, libTOFProc_interface.so) accepts only
raw widths {480, 640, 1280} (the per-plane pixel width). The actual raw data is
**8 phase planes of 1280x480** (= 1280x3840), i.e. exactly the engine's
1280-wide mode — the usecase just passes the blob byte-size instead of the pixel
width (a Samsung version-mismatch quirk on this CHN firmware; this path never
ran in CHN production).

Patch (host file `camera-recon/libTOFProc_interface.patched.so`, deployed to
phone `/opt/tofpatch/libTOFProc_interface.so`, shadowing via LD_LIBRARY_PATH):

```
; set_plugin case 0x28, vaddr 0x15da2 (file off 0x4da2)
- ldr.w r6, [r4, #0xbc]     ; d4 f8 bc 60   (load bogus width 9830472)
+ movw  r6, #0x500          ; 40 f2 00 56   (force the 1280-wide mode)
```

With this the engine accepts the config (`inputRaw rejects: 0`,
`GetProcessResult returns false: 0`) and produces the pointcloud.

### Laser (VCSEL) — required for real data

The VCSEL is NOT auto-enabled by the driver. The shim now fires it in-band:
after the sensor session is acquired (first request), it writes `1` to
`/sys/devices/virtual/camera/rear/rear_tof_ld_onoff` (retries until the store
sticks — it returns EPERM while `g_s_ctrl_tof` is NULL pre-acquire). Log marker
`TOF_LASER VCSEL on (frame N)`. The first ~2-5 frames before the laser ramps are
dark; later frames carry real depth.

## Verification

- request chain: 0 mmap/CSLMap/SubmitChiRequest failures; clean flush/close, rc=0.
- frames: 640x480 pointcloud, 307149/307200 valid points (99.97%).
- depth range in test scene 0.99-1.40 m, temporally stable across frames
  (mean 1.316, median 1.344 both frames 30 & 40).
- rendered depth map: `camera-recon/tof_final_depth.png` (near object top-right
  ~1.0 m warm, far surface ~1.4 m dark) + `tof_depth.png` / `tof_depth.pgm`.
- sample raw frames: `tof_pointcloud_frame30.raw`, `tof_final_frame35.raw`.

## Notes / follow-ups

- Depth16 (0x20363159 ds0x1000) + confidence streams are accepted by the usecase
  too (2-stream mode `T` exists in the shim) but the pointcloud alone already
  gives full xyz depth; depth16 not needed for a depth map.
- The engine also loads the EEPROM dual-tilt calibration (logged at init) and the
  uni-plugin reads `/sys/class/camera/rear/rear_tof_ae_value` per frame.
- The `TOFPointCloudOut1_IFE0 invalid type streamConfig` CamX message persists
  but is benign (the node's own buffer manager is unused; ours serves the port).
- Remaining cosmetic: the usecase teardown still has the known immortal-thread
  Scudo race (cameraserver is SIGKILLed on stock; harmless here, shim _exit()s).
