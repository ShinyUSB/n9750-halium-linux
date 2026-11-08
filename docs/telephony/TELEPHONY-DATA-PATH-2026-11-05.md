# Telephony / mobile-data bring-up on the d2q (Note 10+, SM8150, Debian) — 2026-11-05

The full investigation: the modem works and is registered on the carrier, the
kernel data path (IPA) was dead because of a missing boot trigger, and the
remaining blocker is the IPA pipe setup that stock `netmgrd` performs.

## TL;DR

- The modem (SM8150 X24) is fully up over QRTR: registered on **MTS (Belarus,
  MCC 257 / MNC 2)**, CS+PS attached, LTE signal fine, IMEI present, SIM (USIM)
  detected. ModemManager 1.20.4 detects it (qcom-soc plugin over QRTR).
- **Root cause of the dead data path: the Android early-boot `write /dev/ipa 1`
  action was missing on Debian.** Confirmed in the local kernel source and fixed
  with a persistent systemd trigger. After `echo 1 > /dev/ipa`: the IPA firmware
  loads, `ipa3_post_init` runs, the IPA↔modem QMI handshake completes, and
  `rmnet_ipa0` is created.
- **Remaining blocker**: the IPA data *pipes* are never connected because the
  rmnet ioctls (`ADD_MUX_CHANNEL`, `SET_EGRESS/INGRESS_DATA_FORMAT`) are never
  called — on stock, `netmgrd` (via `librmnetctl.so`) does this. Without them the
  pipes are `-1` → "pipe is not valid" → the modem rejects `WDS Bind Mux Data
  Port` with `Internal` (error 3).

## Phase 1 — the IPA boot trigger (the user's research finding; CONFIRMED + FIXED)

The user found that SM8150 Android userspace does `write /dev/ipa 1` at
early-boot (e.g. OnePlus `vendor/oneplus_sm8150-common` `ipa_fws.rc`; the d2q's
own `/vendor/etc/init/ipa_fws.rc` has it verbatim: `on early-boot / write
/dev/ipa 1`).

Verified in the **local** kernel source (ShinyKernel, `drivers/platform/msm/ipa/ipa_v3/ipa.c`):
- `ipa3_write()` (line 6681): on an MSM device, a write to `/dev/ipa` sets
  `ipa3_ctx->fw_loaded = true` (one-shot guard) and queues `ipa3_fw_loading_work`.
- `ipa3_load_ipa_fw()` → `ipa3_attach_to_smmu()` → `ipa3_pil_load_ipa_fws("ipa_fws")`
  → `ipa3_post_init()` → fires the IPA-ready callbacks.
- The `rmnet_ipa3` probe defers the `rmnet_ipa0` netdev creation until the IPA is
  ready (`ipa_register_ipa_ready_cb`).
- **Race check (the user's concern):** `ipa3_ctx` (probe line 6962) and the
  firmware-load workqueue (7233) are both set up BEFORE `device_create` makes
  `/dev/ipa` appear (7382) — so firing the instant the node appears is safe.

**The missing trigger was confirmed on Debian**: `/dev/ipa` existed
(`crw 244,0`), the trigger never ran (no IPA firmware logs), `/sys/kernel/debug/ipa`
was empty (post_init never ran), and `rmnet_ipa0` never existed. No Debian
systemd unit / udev rule / boot script referenced `/dev/ipa`.

### The fix (installed + verified)

`/etc/systemd/system/d2q-ipa-fw.path`:
```ini
[Unit]
Description=Trigger the IPA firmware load when /dev/ipa appears (modem data path)
[Path]
PathExists=/dev/ipa
Unit=d2q-ipa-fw.service
[Install]
WantedBy=sysinit.target
```
`/etc/systemd/system/d2q-ipa-fw.service`:
```ini
[Unit]
Description=Load the IPA firmware (creates rmnet_ipa0 for the modem data path)
[Service]
Type=oneshot
ExecStart=/bin/sh -c 'echo 1 > /dev/ipa'
RemainAfterExit=yes
[Install]
WantedBy=sysinit.target
```
`systemctl enable d2q-ipa-fw.path` — fires once per boot when `/dev/ipa` appears;
the kernel's `fw_loaded` guard makes any re-write a no-op (no loop possible).

After a manual `echo 1 > /dev/ipa` (the proof):
```
IPA FW loaded successfully
IPA driver initialization was successful.
rmnet_ipa completed initialization
Sending QMI_IPA_INIT_MODEM_DRIVER_REQ_V01 → response received   ← IPA↔modem handshake
rmnet_ipa0 created, UP, MTU 9216
```

## Phase 2 — ModemManager and the DPM/net-port gap

MM 1.20.4 has automatic DPM Open Port for QRTR+IPA Qualcomm SoCs (reads
`$NET/device/modem/tx_endpoint_id` + `rx_endpoint_id` from sysfs, then
WDA Get/Set Data Format → create mux link → WDS Bind Mux Data Port → WDS Start).

On this 4.14 CAF kernel, `rmnet_ipa0` is a **virtual** net device
(`/sys/devices/virtual/net/rmnet_ipa0`) with **no driver link and no
`tx_endpoint_id`/`rx_endpoint_id` sysfs attributes** (the `rmnet_ipa` driver never
creates them). MM's qcom-soc plugin maps net-driver `"ipa"` → embedded endpoint
(mm-modem-helpers-qmi.c:1805) and reads those attrs for the DPM — both are absent,
so MM reports **"Failed to find a net port in the QMI modem"** (mm-base-modem.c:1515)
and disposes the modem. This is a kernel-interface gap, not a config issue.

## Phase 3 — the staged live test (isolated the real blocker)

Per the user's protocol (no combined WDS calls, one config, capture on failure):

| step | result |
|---|---|
| `ip link add link rmnet_ipa0 name rmnet_data1 type rmnet mux_id 1` | ✅ rmnet child created |
| `ip link set rmnet_data1 up` | ✅ UP/LOWER_UP — but kernel logged `ipa3_wwan_xmit: fatal: pipe is not valid` |
| `qmicli --wds-bind-mux-data-port="mux-id=1,ep-type=embedded,ep-iface-number=1" --client-no-release-cid` | ❌ modem: `FAILURE: Internal` (err 3), well-formed request |

**Conclusion**: the bind is not the missing op by itself — the IPA data pipe for
the mux'd path is never connected, so the modem rejects the bind.

## Phase 4 — the actual missing piece: the rmnet ioctls (ipacm/netmgrd's job)

The "pipe is not valid" root cause, verified in the local source + live device:

The IPA data pipes are created ONLY by the rmnet ioctls on `rmnet_ipa0`
(the `ipa3_wwan_ioctl` ndo, `RMNET_IOCTL_EXTENDED = 0x89FD`):

| ioctl | sub | handler | effect |
|---|---|---|---|
| ADD_MUX_CHANNEL | 0x5 | rmnet_ipa.c | register mux_id→name in `mux_channel[]` (REQUIRED beyond RTM_NEWLINK) |
| SET_EGRESS_DATA_FORMAT | 0x6 | `handle3_egress_format` | `mode.dst = IPA_CLIENT_APPS_WAN_PROD` → `ipa3_setup_sys_pipe()` → **`apps_to_ipa3_hdl`** (TX pipe) |
| SET_INGRESS_DATA_FORMAT | 0x7 | `handle3_ingress_format` | `client = IPA_CLIENT_APPS_WAN_CONS` → `ipa3_setup_sys_pipe()` → **`ipa3_to_apps_hdl`** (RX pipe) + QMAP hdr + WAN rt tables |

Format-flag bitmasks:
- egress: `MAP(1<<1) AGGREGATION(1<<2) MUXING(1<<3) CHECKSUM(1<<4) IP_ROUTE(1<<5)`
- ingress: `MAP(1<<1) DEAGGREGATION(1<<2) DEMUXING(1<<3) CHECKSUM(1<<4) AGG_DATA(1<<5) IP_ROUTE(1<<6)`

On stock, **netmgrd** calls these via `/vendor/lib/librmnetctl.so`
(`rmnet_new_vnd`, `rmnet_set_link_egress_data_format`, `rmnet_associate_network_device`,
`rmnet_add_del_vnd_tc_flow`…). Debian has no netmgrd → the pipes never get set up.

**Read-only live inspection** (`rmnet-inspect` tool, GET-only ioctls on rmnet_ipa0):
- `SUPPORTED_FEATURES = 0x7` (mux + egress + ingress formats supported)
- `DRIVER_NAME = rmnet_ipa0`
- `EPID = 1`
- **`EP_PAIR: consumer=2, producer=11`** (the IPA pipe indices for the wwan path)

## Phase 5 — the WDA/QMAP handshake (separate concern, per the user's note)

The local WDA impl (`wda_svc_config`, drivers/soc/qcom/wda_qmi.c:320) sends ONLY
**WDA_SET_POWERSAVE_CONFIG (0x002D)** — a power-management handshake, NOT
WDA_SET_DATA_FORMAT. It's triggered by the `NLMSG_CLIENT_SETUP` netlink flow
(helper `rmnet-flow.c`, implemented + dry-run-verified, NOT executed per
instruction). The data-format/endpoint config on this kernel is carried by the
IPA QMI service (`QMI_IPA_INIT_MODEM_DRIVER_REQ`), which already works.

## The correct data-call sequence (verified against stock + local kernel)

1. IPA init via `/dev/ipa` trigger (done, persistent).
2. `rmnet_ipa0` created (the lower/real device; NOT rmnet-kind — plain
   `ipa3_wwan_ops_ip` netdev_ops, no rtnl_link_ops).
3. Create logical rmnet child `rmnet_dataN` on `rmnet_ipa0` (mux_id N).
4. `ADD_MUX_CHANNEL` + `SET_EGRESS_DATA_FORMAT` + `SET_INGRESS_DATA_FORMAT` on
   `rmnet_ipa0` → connects the IPA pipes.
5. `WDS Bind Mux Data Port` (qmicli 1.32.2 supports it: `mux-id`, `ep-type=embedded`,
   `ep-iface-number`) then `WDS Start Network` (separate qmicli calls).

## The recommended minimal fix (not yet executed)

A small `rmnet-setup` userspace tool doing steps 3–4 via `RMNET_IOCTL_EXTENDED`
on `rmnet_ipa0` (the same mechanism the verified read-only `rmnet-inspect` tool
uses), then the qmicli bind+start. The exact QMAP egress/ingress flag bitmask is
the one value to confirm from a stock boot strace of netmgrd's ioctls (or the
netmgrd disasm) before running live, to avoid a mis-framed pipe.

## Tools produced (host camera-recon/, phone /tmp or /usr/local/bin)

- `rmnet-inspect.c` — read-only rmnet extended-ioctl inspector (verified live).
- `rmnet-flow.c` — the NLMSG_CLIENT_SETUP/QMAP flow-setup helper (dry-run verified;
  NOT executed on the phone).
- `qrtr-scan` (Go) — QRTR service scanner.

## Safety record

- One hard crash: an `rmnet_ipa3` driver unbind/rebind hung/rebooted the phone.
  **Never unbind/rebind rmnet_ipa3 or reload the IPA driver.** No modem/subsystem
  restarts were used. The `/dev/ipa` write is one-shot (kernel `fw_loaded` guard).
- All investigation after that was read-only or the single approved live step.

## What remains

1. Confirm the exact QMAP egress/ingress flag bitmask (stock boot strace or
   netmgrd disasm of `rmnet_set_link_*_data_format`).
2. Implement `rmnet-setup` (steps 3–4) and validate: pipes connected
   (`ipa3_wwan_xmit` stops erroring), bind succeeds, `WDS start-network` gets an
   IP via `WDS Get Current Settings`.
3. Then ModemManager integration (the endpoint-id sysfs gap is a separate,
   later kernel-side nicety — MM isn't required for a working data call).
4. SMS + voice calls are separate follow-ons (WMS / the call audio path).
