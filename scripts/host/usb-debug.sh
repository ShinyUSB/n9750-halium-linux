#!/usr/bin/env bash
# Observe USB/RNDIS state on the Linux host and optionally address the host end
# of the link. This script never flashes, never repartitions, and never erases.
#
# Read-only by default. Pass --setup to add the documented host address to the
# detected RNDIS interface. The interface name is detected, never hard-coded.
set -Eeuo pipefail

DEVICE_IP=${DEVICE_IP:-10.15.19.82}
HOST_IP=${HOST_IP:-10.15.19.1}
HOST_CIDR=${HOST_CIDR:-24}
SETUP=0

usage() {
    cat <<'EOF'
Usage: tools/usb-debug.sh [--setup] [--host-ip A.B.C.D]

Without --setup this only observes interfaces, kernel messages and reachability.
With --setup it also adds the documented host address to the detected RNDIS
interface.

  device : 10.15.19.82/24   (set by tools/build-rootfs-arm64.sh)
  host   : 10.15.19.1/24    (assigned by this script with --setup)

The host interface name is discovered at runtime: f_rndis/u_ether produce usb0,
some kernels produce rndis0, and distributions with predictable naming produce
enx<MAC>. Nothing is hard-coded.
EOF
}

while (($#)); do
    case "$1" in
        --setup) SETUP=1; shift ;;
        --host-ip) HOST_IP=${2:-}; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'error: unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done

command -v ip >/dev/null 2>&1 || { printf 'error: iproute2 is missing\n' >&2; exit 1; }

printf '=== host network interfaces ===\n'
ip -br addr

printf '\n=== candidate RNDIS interfaces ===\n'
# Match the names f_rndis/u_ether actually produce rather than a fixed one.
IFACE=$(ip -br link | awk '{print $1" "$2}' \
    | grep -Ei '(rndis|usb[0-9]|enx[0-9a-f]{6})' | awk '{print $1}' | head -n 1 || true)

if [[ -z "$IFACE" ]]; then
    printf 'No RNDIS interface found yet.\n'
    printf 'Wait for the phone to enumerate, or check the kernel log for:\n'
    printf '  usb 1-1: new high-speed USB device\n'
    printf '  usbhid: ... RNDIS ...\n'
else
    printf 'detected RNDIS interface: %s\n' "$IFACE"
    ip -br addr show dev "$IFACE"

    if ((SETUP)); then
        printf '\n==> Adding %s/%s to %s\n' "$HOST_IP" "$HOST_CIDR" "$IFACE"
        ip addr replace "$HOST_IP/$HOST_CIDR" dev "$IFACE"
        ip link set dev "$IFACE" up
        ip -br addr show dev "$IFACE"
    else
        printf '\nRe-run with --setup to add %s/%s to %s\n' "$HOST_IP" "$HOST_CIDR" "$IFACE"
    fi
fi

printf '\n=== USB/RNDIS kernel messages (last 200) ===\n'
dmesg --ctime 2>/dev/null \
    | grep -Ei 'usb|rndis|dwc3|udc|configfs|ether' | tail -200 || true

printf '\n=== routing to %s ===\n' "$DEVICE_IP"
ip route get "$DEVICE_IP" 2>&1 || true

printf '\n=== connectivity probe: %s ===\n' "$DEVICE_IP"
ping -c 3 -W 2 "$DEVICE_IP" || printf 'no reply yet (this is expected before first boot)\n'

cat <<EOF

Next:
  tools/ssh-device.sh                      # ssh root@$DEVICE_IP
On the phone, if reachable:
  dmesg | grep -Ei "usb|rndis|dwc3|configfs"
  ip -br addr
  systemctl status d2q-rndis ssh
EOF
