#!/usr/bin/env bash
# Open the documented headless SSH endpoint without guessing a host USB name.
#
# The device address is fixed by tools/build-rootfs-arm64.sh (10.15.19.82/24).
# The host side of the link is never assumed: this script only needs a route to
# that address, which means addressing the host end of the USB Ethernet link
# first (see tools/usb-debug.sh --setup, which assigns 10.15.19.1/24).
set -Eeuo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

DEVICE_IP=${DEVICE_IP:-10.15.19.82}
PORT=${PORT:-22}
KEY=${KEY:-"$ROOT/artifacts/keys/d2q_rsa"}

# The headless rootfs has key-based root login only: there is no root password
# and no console to set one into on a device with no display.
if [[ -f "$KEY" ]]; then
    exec ssh -i "$KEY" -o IdentitiesOnly=yes -p "$PORT" "root@$DEVICE_IP" "$@"
fi

printf 'note: %s not found, falling back to the default ssh identity\n' "$KEY" >&2
printf 'note: run tools/build-rootfs-arm64.sh first for key-based root login\n' >&2
exec ssh -p "$PORT" "root@$DEVICE_IP" "$@"
