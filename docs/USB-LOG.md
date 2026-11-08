# USB host mode + physical keyboard on SM-N9750 (D2Q, Debian)

Goal: make a USB HID keyboard work on the phone. Suspected blocker was "forced
RNDIS" monopolising the USB port.

## Result: keyboard works

    I: Bus=0003 Vendor=1c4f Product=0084 Version=0111
    N: Name="ZXZK  RS0-1 61 USB Keyboard"
    P: Phys=usb-xhci-hcd.8.auto-1/input1
    B: EV=10001f
    H: Handlers=sysrq sec_key_notifier kbd event12

libdev path: `/dev/input/by-id/usb-ZXZK_RS0-1_61_USB_Keyboard-event-kbd`

Weston picked it up live on hotplug:

    event12 - ZXZK  RS0-1 61 USB Keyboard: is tagged by udev as: Keyboard
    event12 - ZXZK  RS0-1 61 USB Keyboard: device is a keyboard
    libinput: configuring device "ZXZK  RS0-1 61 USB Keyboard".
    associating input device event12 with output DSI-1

## USB topology (important)

There is **one** USB port, and the DWC3 controller exposes both faces:

- `a600000.dwc3` — the UDC (device/gadget) face, used for RNDIS.
- `usb1` = `xhci-hcd.8.auto` — the host face of the *same* DWC3 (`ls
  /sys/bus/platform/devices/a600000.dwc3/` shows both `udc` and `xhci-hcd.8.auto`).

So the flash drive / keyboard on `usb1` and RNDIS on the UDC are the same
physical port. Disabling the gadget does not remove host functionality - both can
coexist, and after unbinding the gadget the host side kept working.

## RNDIS disabled

`d2q-rndis.service` (in `/lib/systemd/system/`) creates the configfs gadget `d2q`
and binds it to the UDC. Disabling it:

    systemctl disable --now d2q-rndis
    echo "" > /sys/kernel/config/usb_gadget/d2q/UDC    # only if dir still exists

Result: `rndis0` disappears, UDC reports `not attached`, `usb1`/host mode
unaffected. `10-d2q-rndis.network` (static 10.15.19.82/24) is left in place but
inert - harmless, and convenient if RNDIS is ever wanted again.

## "Turn on power to the external device" - nothing to do

There is **no VBUS switch** to flip on this board. I looked for:
`/sys/class/usb_role` (absent), `regulator_summary` USB/VBUS entries (only power
domains `usb30_prim_gdsc`/`usb30_sec_gdsc`, no VBUS regulator), DT nodes matching
`vbus`/`usb host`/`otg` (none), and gpio lines labelled usb/vbus (none).

VBUS is supplied unconditionally by the port hardware. Proof: the flash drive
enumerated and responded:

    dd if=/dev/sde bs=512 count=1
    1+0 records in / 1+0 records out / 512 bytes copied, 0.0012424 s, 412 kB/s

A USB device cannot answer at all without VBUS, so power was always fine.

**Correction to an earlier assumption:** the flash drive appeared dead because it
is formatted **exFAT** and this kernel has no exFAT module
(`modprobe exfat` -> `Module exfat not found`; `/proc/filesystems` has only `ntfs`
and `fuse`). That is a filesystem-support gap, not a power problem. The BT dongle
on the host is a different port and was never involved.

## Getting the phone onto the host's subnet

- Host: `enp3s0` = 192.168.1.8/24, router 192.168.1.1 = `3c:46:45:84:34:6c`.
- Target SSID `WLAN_2.4G_346C` is that same router, and serves DHCP -> the phone
  gets **192.168.1.16**.
- `/etc/wpa_supplicant.conf` was rewritten so `WLAN_2.4G_346C` is priority 20 and
  is tried first; `WLAN_5G_346C` (priority 5) and `OpenWrt_1` (priority 1) remain
  as fallbacks. Note `OpenWrt_1` is a *different* subnet (192.168.2.x), which is
  why the phone was previously unreachable from the host without routing.
  Backup: `/etc/wpa_supplicant.conf.bak-<timestamp>`.
  Local copy: `artifacts/wpa_supplicant.conf`.

## Quirk: the phone does not answer ARP for its own address

Symptom: phone -> host works, host -> phone fails with `No route to host`, and
`ip neigh` on the host goes `FAILED` for the phone's address. `arping` from the
host gets 0 replies. The phone's *ARP requests* work fine (it learned the host's
MAC), only its *replies* are suppressed.

Workaround: pin the mapping on the host.

    sudo ip neigh replace 192.168.1.16 lladdr 00:13:e0:fd:41:2a dev enp3s0 nud permanent

With that, ping and SSH from the host both work. Automated by
`tools/phone-wlan-arp.sh` (also appended to the host's `/etc/rc.local`).
The phone's wlan0 MAC is `00:13:e0:fd:41:2a` - re-check it if the DHCP lease
changes, since the script takes both values as env vars.

## Current service state

| unit | state | why |
|---|---|---|
| `d2q-rndis` | disabled | port is in host mode; USB network not used |
| `d2q-wlan` | enabled | brings up wpa_supplicant; picks 2.4G_346C by priority |
| `weston` | enabled | landscape, no idle-blank, sees USB keyboard |
| `bt-mouse` | enabled | internal BT mouse |
| `bt-uart-pm` / `bt-uart-pm-guard` | enabled | keep QUP UART out of runtime PM |

SSH endpoints: `10.15.19.82` (RNDIS, now gone) and `192.168.1.16` (WLAN). Use
`DEVICE_IP=192.168.1.16 ./tools/ssh-device.sh '<cmd>'`.

## If the flash drive is wanted

Needs exFAT support, not power:

    apt install exfatprogs    # userspace mount, uses the kernel driver... which is absent
    # so on this kernel either:
    #  a) rebuild the kernel with CONFIG_EXFAT_FS=y, or
    #  b) reformat the drive as ext4/fat32, or
    #  c) use a userspace exfat implementation on top of fuse