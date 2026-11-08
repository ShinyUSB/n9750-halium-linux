# Android Property Subsystem Fix — Root Cause Report
**Date: 2026-11-01 · d2q (SM-N9750) Debian · camera bring-up**

## TL;DR

The Android 12 Bionic was **never** rejecting the Android 15 GSI property area on format
grounds. Every file in `/dev/__properties__/` was owned **uid=1000 gid=1000** (numeric IDs
preserved by tar extraction as a non-root user), and Bionic's property loader hard-requires
`st_uid == 0 && st_gid == 0`. It failed the metadata gate and closed the fd **before mmap** —
exactly the `openat → fstat64 → close` pattern we observed.

**The entire fix was `chown -R 0:0 /dev/__properties__`.** After it, `configure_streams`
succeeds, the session is created, `process_capture_request` returns 0, and **real camera
frames are captured on Debian** (see `frame0_first_capture.png` — Y plane of a 4032×3024
YCbCr_420_888 capture, actual scene content: window frame/wall).

---

## A. Root cause

### Observed syscall sequence (strace of proptest, before fix)

```
openat(AT_FDCWD, "/dev/__properties__/property_info",
       O_RDONLY|O_LARGEFILE|O_NOFOLLOW|O_CLOEXEC) = 3
fstat64(3, 0xff8dea90)                          = 0
close(3)                                        = 0
```

No `mmap()`. No `read()` (expected — the file is mapped, not read).

### Failing check

`PropertyInfoAreaFile::LoadPath()`
(system/core/property_service/libpropertyinfoparser/property_info_parser.cpp,
linked into Bionic for `ContextsSerialized`):

```cpp
if ((fd_stat.st_uid != 0) || (fd_stat.st_gid != 0) ||
    ((fd_stat.st_mode & (S_IWGRP | S_IWOTH)) != 0) ||
    (fd_stat.st_size < static_cast<off_t>(sizeof(PropertyInfoArea)))) {
  close(fd);
  return false;            // <-- we exited here: uid=1000, gid=1000
}
```

`ContextsSerialized::Initialize()` propagates the failure, `SystemProperties::Init()`
returns false, `__system_property_area_init()` never completes, and **every** property read
in the process returns empty — not just the camera props. That is why CamX/CHI produced
degenerate stream configs ("CreateBufferManagers: invalid type streamConfig" on every node)
and the session was never created.

Note the task's mental model was right about the *selection logic* (presence of
`property_info` → `ContextsSerialized`, no fallback to `ContextsSplit`), but the rejection
was **ownership**, not Android-15-vs-12 serialization.

### Secondary gate (same root cause)

Per-context area files (`u:object_r:*:s0`) go through `prop_area::map_prop_area()`
(bionic prop_area.cpp), which applies the **same uid==0/gid==0/not-writable check**.
So `chown -R 0:0` on the whole tree (not just `property_info`) is required. Modes were
already fine (`r--r--r--` files, `drwx--x--x` dirs).

## B. Compatibility analysis — Android 12 Bionic reader vs Android 15 GSI writer

Measured from the actual files on the phone (`od -A x -t x4`):

**`/dev/__properties__/property_info` (PropertyInfoArea header):**

| Field | Value | Android 12 reader requirement | Verdict |
|---|---|---|---|
| current_version | 1 | — | OK |
| minimum_supported_version | 1 | ≤ 1 | OK |
| size | 0x1c0c4 = 114884 | == file size (114884) | OK |
| contexts_offset / types_offset / root_offset | 0x18 / 0x493c / 0x4c1c | in-bounds | OK |

**`/dev/__properties__/properties_serial` (prop_area header):**

| Field | Value | Expected | Verdict |
|---|---|---|---|
| magic @ +8 | 0x504f5250 ("PROP") | PROP_AREA_MAGIC | OK |
| version @ +12 | 0xfc6ed0ab | PROP_AREA_VERSION | OK |
| area size | 131072 (128 KiB default) | — | OK |

**Conclusion: the Android 15 GSI property files are byte-format-compatible with the
Android 12 Bionic reader.** No re-serialization, no custom trie builder, no Android 12
property_info regeneration was needed. The `ContextsSplit` fallback experiment is moot.

Directory layout used (serialized context model, Android 12 Bionic):

```
/dev/__properties__/property_info            name→context trie (validated, mmap'd)
/dev/__properties__/properties_serial        legacy/serial context area
/dev/__properties__/u:object_r:<ctx>:s0      one 128 KiB prop_area per SELinux context
/dev/__properties__/appcompat_override/...   (untouched, not used by this stack)
```

## C. Working implementation

Smallest correct solution — no new code, no serializer:

1. Fix ownership once: `chown -R 0:0 /dev/__properties__`
2. Make it persistent (phone `/dev` is **devtmpfs** — contents vanish at reboot):
   - Canonical copy: `/opt/property-area/` (root:root, modes preserved, snapshot of the
     fixed live tree)
   - Boot unit: `/etc/systemd/system/d2q-props.service` (enabled, `Before=sysinit.target`):

     ```
     ExecStart=/bin/sh -c "mkdir -p /dev/__properties__ && cp -a /opt/property-area/. /dev/__properties__/"
     ```

Static read-only property area is **sufficient** — no property service / runtime set
support was needed by the camera stack.

Pristine originals preserved untouched: host `camera-recon/props/area/` (uid 1000) and
`camera-recon/props/props.tar`.

## D. Standalone Bionic test results

Tools (source in camera-recon/, built with the same
`clang --target=armv7a-linux-androideabi -mthumb -nostdlib -fuse-ld=lld` + bionic32 libs,
run via `/opt/bionic32/linker`):

**proptest** (`__system_property_find`/`__system_property_get`):

```
[pt] ro.hardware: found=yes rc=4 val='qcom'
[pt] persist.vendor.camera.sync_en: found=no rc=0 val=''
[pt] persist.vendor.camera.multiframe_en: found=no rc=0 val=''
[pt] persist.vendor.camera.mfnr_en: found=no rc=0 val=''
[pt] persist.vendor.camera.fastaec.enable: found=no rc=0 val=''
```

**propdump** (`__system_property_foreach` + `__system_property_read_callback`):
**1650 properties resolve** through the real Android 12 Bionic reader (was: zero).

## E. Camera validation

`cam-shim32` full run after the fix:

```
[shim] number_of_cameras = 5
[shim] open camera 0 rc=0 dev=... ver=773
[shim] initialize rc=0
[shim] configure_streams rc=0 (stream: w=1440 h=1080 fmt=0x23 usage=0x20000 maxbuf=8)
[shim] capture req 1..4 rc=0          <-- no more EINVAL from null session
[cb] result frame=1..4 outputs=1      <-- real output buffers delivered
[shim] dumped 24385536 bytes to /tmp/frame0.raw
[shim] DONE
```

- **"CreateBufferManagers: invalid type streamConfig": 0 occurrences** (was: every node).
- `device->session` is created; `process_capture_request()` returns 0 for all 4 requests.
- Frame content verified visually: `frame0_first_capture.png` (Y plane, 4032×3024) shows a
  real scene. ~75% nonzero samples; smooth luminance gradients.

## F. persist.vendor.camera.* properties — disposition

The CHI override reads 12 `persist.vendor.camera.*` props (from strings in
com.samsung.chi.override.so): `sync_en, multiframe_en, mfnr_en, fastaec.enable, lpm_en,
aec_mfnr_swonoff, cam_id.change, debug.level, enablePerfLock, livefocus.jpegCam,
livefocus.lfc_tof, tof.videobokeh`.

**None exist in the GSI dump** (only `persist.vendor.camera.adjustfps=` empty). Stock values
are unrecoverable locally: the userdata partition's `/data/property/` was wiped to halium
props only. **They were not populated and not needed**: the pipeline works on CHI/CamX
built-in defaults (the root cause was never missing camera props — it was the wholesale
property-subsystem rejection making *all* reads empty). If a future usecase requires a
specific value, props must be added by rebuilding the `u:object_r:vendor_camera_prop:s0`
area with the AOSP serializer (writable `prop_area` path) — do not handcraft, do not guess
values; get them from a stock boot via `adb shell getprop` first.

`vendor_property_contexts` confirms `persist.vendor.camera.` → `u:object_r:vendor_camera_prop:s0`.

## G. Final architecture / boot order

| Item | Location | Notes |
|---|---|---|
| Canonical property tree | phone `/opt/property-area/` | root:root, files 444, dirs 711 |
| Deploy unit | `d2q-props.service` (enabled) | cp -a into devtmpfs `/dev/__properties__`, Before=sysinit.target |
| Runtime area | `/dev/__properties__/` | volatile; rebuilt each boot by the unit |
| Bionic env | `/opt/bionic32/` (libc, linker, support libs) | unchanged |
| Test tools | phone `/usr/local/bin/{proptest,propdump}`; host `camera-recon/{proptest.c,propdump.c}` | |
| SELinux | not enforced on Debian; context strings are just filename components | no MAC gating needed |

Startup order: `d2q-props.service` (sysinit) → camera stack on demand. No helper daemon
needed; read-only area suffices.

## Known remaining issue (out of scope, non-blocking)

Process exits with SIGABRT during static teardown **after** `[shim] DONE` (main thread
self-`abort()` in the exit path; all 4 frames already delivered). Likely a CamX global
destructor asserting state the shim never set up (shim never calls `flush()`/device close).
Capture flow is unaffected.
