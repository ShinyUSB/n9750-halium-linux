#!/usr/bin/env bash
# Host side of the initramfs-to-rootfs transfer.
#
# The Halium panic handler exposes RNDIS at 192.168.2.15 and a root telnet shell
# on port 23, and /tmpmnt (the mounted userdata) is writable in that shell. This
# script serves rootfs.img over HTTP so the phone can pull it with busybox wget,
# which avoids needing Android, ADB, Odin, or any repartitioning.
#
# Nothing here writes to the phone; the phone fetches. The operator runs the
# wget command printed at the end, from the phone's telnet shell.
set -Eeuo pipefail
export LC_ALL=C

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

IMAGE=${IMAGE:-"$ROOT/artifacts/flash/03-rootfs-arm64.img"}
LINK_IP=${LINK_IP:-192.168.2.100}      # host end of the initramfs RNDIS link
LINK_PREFIX=${LINK_PREFIX:-24}
DEVICE_IP=${DEVICE_IP:-192.168.2.15}    # the initramfs panic telnet address
TELNET_PORT=${TELNET_PORT:-23}
HTTP_PORT=${HTTP_PORT:-8000}

usage() {
    cat <<'EOF'
Usage: tools/initrd-fetch-host.sh [--image FILE] [--no-serve]

Prepares the host end of the RNDIS link the Halium panic handler creates, and
serves rootfs.img over HTTP so the device can pull it.

  --image FILE   image to serve (default: artifacts/flash/03-rootfs-arm64.img)
  --no-serve     set up the link but do not start the HTTP server

Then, from the phone's telnet shell (telnet 192.168.2.15 23):

  wget http://192.168.2.100:8000/<image-name> -O /tmpmnt/rootfs.img
  sha256sum /tmpmnt/rootfs.img
  sync

This script only touches the host. It never writes to the device.
EOF
}

SERVE=1
while (($#)); do
    case "$1" in
        --image) IMAGE=$2; shift 2 ;;
        --no-serve) SERVE=0; shift ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'error: unknown argument: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done

fail() { printf 'error: %s\n' "$*" >&2; exit 1; }
note() { printf '==> %s\n' "$*"; }

command -v ip >/dev/null 2>&1 || fail "iproute2 is missing"
[[ -f "$IMAGE" ]] || fail "image not found: $IMAGE"
command -v sha256sum >/dev/null 2>&1 || fail "sha256sum is missing"
command -v python3 >/dev/null 2>&1 || fail "python3 is missing (needed for the HTTP server)"

# The device side always lands on /data/rootfs.img, whatever the file is called.
DEST_NAME=$(basename "$IMAGE")
SUM=$(sha256sum "$IMAGE" | cut -d' ' -f1)
SIZE=$(stat -c %s "$IMAGE")

# The panic script names the interface rndis0 or usb0 depending on the kernel.
# Discover it rather than assuming; nothing is hard-coded. The `|| true` matters:
# with `set -e` a failed grep in the pipeline would abort before we could explain.
IFACE=$(ip -br link | awk '{print $1" "$2}' \
    | grep -Ei '^(rndis0|usb0|enx[0-9a-f]{6}) ' | awk '{print $1}' | head -n 1) || true

if [[ -z "$IFACE" ]]; then
    note "no RNDIS interface found yet."
    note "The Halium panic handler creates one only after the initramfs panics,"
    note "which happens when /data/rootfs.img is missing. Watch for it with:"
    note "    watch -n1 'ip -br link | grep -Ei \"rndis|usb0|enx\"'"
    note "Re-run this script once the interface appears."
    exit 1
fi

note "detected initramfs RNDIS interface: $IFACE"

# Assign the host end. The device is already 192.168.2.15/24 from the panic
# script, so a static address on the same subnet is all that is needed; no DHCP
# client is required on the host.
if ip -4 addr show dev "$IFACE" | grep -q "inet $LINK_IP/"; then
    note "host address $LINK_IP/$LINK_PREFIX already present on $IFACE"
else
    note "assigning $LINK_IP/$LINK_PREFIX to $IFACE"
    ip addr replace "$LINK_IP/$LINK_PREFIX" dev "$IFACE"
fi
ip link set dev "$IFACE" up

printf '\n=== link state ===\n'
ip -br addr show dev "$IFACE"

note "device telnet endpoint: $DEVICE_IP:$TELNET_PORT"
if command -v timeout >/dev/null 2>&1; then
    if timeout 5 bash -c "cat < /dev/null > /dev/tcp/$DEVICE_IP/$TELNET_PORT" 2>/dev/null; then
        note "telnet port $TELNET_PORT on $DEVICE_IP is open"
    else
        note "telnet port $TELNET_PORT not open yet (the panic handler may still be starting)"
    fi
fi

if ((SERVE == 0)); then
    note "not starting the HTTP server (--no-serve)"
    exit 0
fi

cat <<EOF

==> Ready to serve
    image  : $IMAGE
    size   : $SIZE bytes
    sha256 : $SUM
    url    : http://$LINK_IP:$HTTP_PORT/$DEST_NAME
    dest   : /tmpmnt/rootfs.img   (== /data/rootfs.img on the device)

    On the phone, in the telnet shell (telnet $DEVICE_IP $TELNET_PORT):

        wget http://$LINK_IP:$HTTP_PORT/$DEST_NAME -O /tmpmnt/rootfs.img
        sha256sum /tmpmnt/rootfs.img
        sync

    The sha256sum MUST print:
        $SUM

    If it does not match, do not reboot; re-run the wget.

    After a verified match, reboot the phone. The initramfs will then find
    /data/rootfs.img and switch into the rootfs.

==> Starting HTTP server on port $HTTP_PORT (Ctrl-C to stop)
EOF

cd "$(dirname "$IMAGE")"
exec python3 -m http.server "$HTTP_PORT" --bind "$LINK_IP"
