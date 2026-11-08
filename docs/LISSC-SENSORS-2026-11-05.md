# libssc sensor suite work on d2q — 2026-11-05

Gyroscope exposure fix + new pressure/barometer class + proximity crash fix,
validated on the d2q (Note 10+, Debian 12, IMX516/LSM6DSO/AK0991X/LPS22HH/TMD4907).

## Root causes

**A. Gyroscope** — NOT actually missing. The phone already ran libssc **0.4.4** which
contains the full `SSCSensorGyroscope` class and a working `--sensor gyroscope`
dispatch (added upstream in 0.3.0). The real defects were only in the CLI:
the `--sensor` **help text omitted `gyroscope`** (so it looked unsupported), and the
measurement was printed with the wrong unit **`m/s`** (gyroscope = angular velocity,
should be **rad/s**).

**B. Pressure/barometer** — libssc had **no pressure class at all**. The d2q has an
LPS22HH barometer (SSC data type `pressure`) that streams fine over raw SSC but had
no libssc wrapper. Implemented `SSCSensorPressure`.

**C. Proximity crash** — `libssc-sensor-proximity.c` used `g_assert_not_reached()`
in the `default:` case of the `near` enum switch. The Samsung TMD4907 reports an
**out-of-enum `near` value (2)** as its idle/resting state, which aborted the whole
process. Untrusted DSP input must never abort the client. (A second latent crash:
the Xiaomi-Davinci path dereferenced `msg->data` without a NULL/empty check.)

## Changes (focused, reviewable)

Modified:
- `src/libssc-cli.c` — help text now lists `gyroscope` + `pressure`; gyroscope unit
  `m/s` → `rad/s`; added `pressure_close_cb` / `pressure_measurement` + the
  `--sensor pressure` dispatch.
- `src/libssc-cli-private.h` — include `libssc-sensor-pressure.h`.
- `src/libssc-sensor-proximity.c` — the crash fix (see below).
- `data/meson.build` — added `sensor-pressure` to the proto list.
- `src/meson.build` — added `libssc-sensor-pressure.h` / `.c` to headers/sources.
- `src/libssc.h` — umbrella include of the pressure header.
- `libssc.map` — exported the 9 pressure symbols + `ssc_sensor_pressure_get_type`.

New:
- `data/ssc-sensor-pressure.proto` — `SscPressureResponse { repeated float data = 1;
  required int32 status = 2; }` — wire-compatible with the Qualcomm
  `sns_std_sensor_event` (verified against the real LPS22HH byte stream:
  field 1 float = hPa, field 2 varint = sample status).
- `src/libssc-sensor-pressure.h` / `src/libssc-sensor-pressure.c` — the GObject class
  (modeled on `SSCSensorGyroscope`): async+sync new/open/close, a single-float
  `measurement` signal (pressure in **hPa**, no unit conversion), discovery by data
  type `pressure`, SUID-filtered report handler, main-context signal emission.

## Pressure API

`SSCSensorPressure` (final `SSCSensor` subclass):
- `ssc_sensor_pressure_new[_finish|_sync]`, `ssc_sensor_pressure_open[_finish|_sync]`,
  `ssc_sensor_pressure_close[_finish|_sync]` — same lifecycle as the other sensors.
- Signal `measurement (gfloat pressure)` — pressure in **hPa** (data[0] of the
  `sns_std_sensor_event`, per the Qualcomm pressure API).
- Discovery via `SSC_SENSOR_DATA_TYPE, "pressure"`; the report handler filters by the
  sensor's SUID (high+low) and `SSC_MSG_REPORT_MEASUREMENT`.

## Proximity fix

- Replaced both `g_assert_not_reached()` calls (standard + Davinci paths) with
  graceful handling: out-of-enum `near` values are logged and treated as **FAR**
  (the physically-correct reading for a resting proximity sensor) instead of
  aborting — the process never dies on untrusted DSP input, and the NEAR/FAR state
  machine keeps working. The TMD4907's idle `near=2` now yields a clean `FAR` report.
- Guarded the Davinci path against a NULL/empty `msg->data` array before
  dereferencing (`msg->data == NULL || msg->n_data == 0 || len mismatch`).
- Davinci behavior otherwise unchanged; the first-measurement-on-open behavior
  (reported_once reset in `_open`) was already present upstream and is preserved.

## Build & test

- Phone has no toolchain → set up **native ARM64 build on the d2q itself**:
  `apt install gcc meson(pip>=1.4) ninja pkg-config libglib2.0-dev libqmi-glib-dev
  libmbim-glib-dev libprotobuf-c-dev protobuf-c-compiler python3-dev python3-protobuf
  libqrtr1`.
- The system qmi-glib (bookworm 1.32.2) **lacks the SSC service** — built against the
  SSC-enabled qmi-glib 1.39.1 staged at `/opt/qmi-ssc` (headers+lib+pkgconfig;
  `PKG_CONFIG_PATH=/opt/qmi-ssc/lib/pkgconfig`). Source on host:
  `mnt-sda/mesa-root/root/libqmi-main-3454c2c…/`.
- Native ARM64 build: clean. All 10 pressure symbols exported (`nm -D`), ssccli links
  the freshly built `libssc.so.2` + the SSC qmi-glib. Installed to `/opt/libssc`
  (old `libssc.so.2`→`.bak`, old `ssccli`→`ssccli.bak`).
- **Mock unit tests** (`meson test`): all 7 fail on the live phone — but this is
  **pre-existing and unrelated to these changes**: the mock SSC server registers on the
  phone's *real* QRTR bus (node 1) and collides with the live SLPI, and the fixtures/
  assertions (e.g. accel `x==0.0`, proximity `len>=8`) don't hold against the mock's
  actual feed. accel/magnetometer/compass/light fail identically and are code paths I
  did not touch. No hardware test is claimed from these.
- **Hardware validation (the real proof) on the d2q, all via the installed lib:**

| command | result |
|---|---|
| `ssccli --sensor accelerometer` | m/s² (Z≈9.69 flat) ✓ |
| `ssccli --sensor magnetometer`  | µT field ✓ |
| `ssccli --sensor compass`       | ° heading (~170°) ✓ |
| `ssccli --sensor light`         | 0.0 Lux (dark room) ✓ |
| `ssccli --sensor gyroscope`     | **rad/s** ✓ |
| `ssccli --sensor pressure`      | **~1003.9 hPa** ✓ |
| `ssccli --sensor proximity`     | **FAR**, no crash ✓ |

- Repeated open/close (6× pressure + 6× proximity): no crashes, clean enable/disable.

## How to test on d2q

```
export LD_LIBRARY_PATH=/opt/libssc/lib     # ssccli needs the lib dir
ssccli --sensor gyroscope    # X/Y/Z angular velocity in rad/s, continuous
ssccli --sensor pressure     # hPa, continuous
ssccli --sensor proximity    # NEAR/FAR (FAR at idle; cover the sensor for NEAR)
```

## Remaining hardware-specific limitations

- **Proximity NEAR polarity unverified**: the TMD4907 sends `near=2` at idle (treated
  as FAR). A NEAR reading needs a physical cover test by hand — the value-1→NEAR path
  is correct per the Qualcomm enum but I could not trigger it remotely.
- **Light = 0 Lux** in the test environment (dark room); the ALS responds but I had no
  light source to vary it remotely.
- **Barometer via libssc only** — libssc has no pressure class upstream; raw SSC reads
  it fine (the `sensor-read` tool) and now libssc covers it via the new class.
- The mock unit tests need an isolated QRTR environment to run green; they were not
  green on the live phone before this work either.
