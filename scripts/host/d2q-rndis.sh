#!/bin/sh
# Minimal native-rootfs USB RNDIS setup for d2q.
# Run as root inside the booted GNU/Linux rootfs. It does not touch partitions.
set -eu

DEVICE_IP=${DEVICE_IP:-10.15.19.82}
PREFIX=${PREFIX:-24}
MAC=${MAC:-02:11:22:33:44:55}
UDC=${UDC:-}
CONFIGFS=${CONFIGFS:-/sys/kernel/config}
GADGET="$CONFIGFS/usb_gadget/d2q"

cleanup() {
    if [ -e "$GADGET/UDC" ]; then
        : > "$GADGET/UDC" 2>/dev/null || true
    fi
    if [ "${1:-}" = "--cleanup" ]; then
        rm -f "$GADGET/configs/c.1/rndis.usb0" 2>/dev/null || true
        rmdir "$GADGET/configs/c.1/strings/0x409" 2>/dev/null || true
        rmdir "$GADGET/configs/c.1" 2>/dev/null || true
        rmdir "$GADGET/functions/rndis.usb0" 2>/dev/null || true
        rmdir "$GADGET/strings/0x409" 2>/dev/null || true
        rmdir "$GADGET" 2>/dev/null || true
    fi
}

case "${1:-}" in
    --cleanup) cleanup --cleanup; exit 0 ;;
    "") ;;
    *) printf 'usage: %s [--cleanup]\n' "$0" >&2; exit 2 ;;
esac

[ -d "$CONFIGFS" ] || { printf 'error: configfs is not available at %s\n' "$CONFIGFS" >&2; exit 1; }
if ! mountpoint -q "$CONFIGFS"; then
    mount -t configfs configfs "$CONFIGFS"
fi

# Stop Android's legacy USB gadget if this rootfs is co-existing with Android.
if [ -w /sys/class/android_usb/android0/enable ]; then
    printf 0 > /sys/class/android_usb/android0/enable
fi

if [ -z "$UDC" ]; then
    UDC=$(ls /sys/class/udc 2>/dev/null | head -n 1 || true)
fi
[ -n "$UDC" ] || { printf 'error: no USB device controller found\n' >&2; exit 1; }

if [ ! -d "$GADGET" ]; then
    mkdir -p "$GADGET"
    printf '0x18d1\n' > "$GADGET/idVendor"
    printf '0xd001\n' > "$GADGET/idProduct"
    mkdir -p "$GADGET/strings/0x409"
    printf 'GNU/Linux Device\n' > "$GADGET/strings/0x409/manufacturer"
    printf 'Halium d2q\n' > "$GADGET/strings/0x409/product"
    printf 'd2q-headless\n' > "$GADGET/strings/0x409/serialnumber"
    mkdir -p "$GADGET/functions/rndis.usb0"
    mkdir -p "$GADGET/configs/c.1/strings/0x409"
    printf 'RNDIS\n' > "$GADGET/configs/c.1/strings/0x409/configuration"
    ln -s "$GADGET/functions/rndis.usb0" "$GADGET/configs/c.1/rndis.usb0"
    printf '%s\n' "$UDC" > "$GADGET/UDC"
fi

# f_rndis normally creates usb0; accept rndis0 on vendor kernels.
netdev=
for candidate in usb0 rndis0; do
    if ip link show "$candidate" >/dev/null 2>&1; then
        netdev=$candidate
        break
    fi
done
[ -n "$netdev" ] || { printf 'error: RNDIS network interface did not appear\n' >&2; exit 1; }

ip link set dev "$netdev" address "$MAC"
ip address replace "$DEVICE_IP/$PREFIX" dev "$netdev"
ip link set dev "$netdev" up
printf 'RNDIS ready: %s is %s/%s (UDC %s)\n' "$netdev" "$DEVICE_IP" "$PREFIX" "$UDC"
printf 'On the host, use 10.15.19.100/24 and then: ssh root@%s\n' "$DEVICE_IP"
trap 'cleanup' INT TERM
