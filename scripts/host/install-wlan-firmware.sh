#!/usr/bin/env bash
# Install Broadcom BCM4375 WLAN firmware into the Debian rootfs on d2q.
# Source files were pulled from the WORKING Android installation
# (/vendor/firmware) on 2026-09-28; see artifacts/wlan-firmware-android/.
# The bcmdhd_101_16 driver is built with DHD_LINUX_STD_FW_API, so it uses
# standard request_firmware() with auto-concatenated chip suffixes:
#   fw  = "bcmdhd_sta.bin" + "_b1"            -> bcmdhd_sta.bin_b1
#   nv  = "nvram.txt" + "_<vid>_<rev>"        -> e.g. nvram.txt_1rh_es43_b1
#   clm = "bcmdhd_clm.blob"
# All known nvram variants are installed; the driver selects by chip VID.
# Usage: tools/install-wlan-firmware.sh [--execute]
# Default is dry-run (lists what would be copied).
set -Eeuo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
FW_DIR=${FW_DIR:-"$ROOT/artifacts/wlan-firmware-android"}
SSH_KEY=${SSH_KEY:-"$ROOT/artifacts/keys/d2q_rsa"}
DEVICE=${DEVICE:-root@10.15.19.82}
EXECUTE=0

[[ "${1:-}" == "--execute" ]] || {
    echo "Dry-run. Files that would be installed to $DEVICE:/lib/firmware/:"
    ls "$FW_DIR" | grep -v SHA256SUMS
    echo "Re-run with --execute after the device boots Debian with SSH."
    exit 0
}

echo "==> Copying firmware to $DEVICE:/lib/firmware/"
ssh -i "$SSH_KEY" -o StrictHostKeyChecking=no "$DEVICE" 'mkdir -p /tmp/wlanfw /lib/firmware'
scp -i "$SSH_KEY" -o StrictHostKeyChecking=no \
    "$FW_DIR"/bcmdhd_sta.bin_b1 \
    "$FW_DIR"/bcmdhd_mon.bin_b1 \
    "$FW_DIR"/bcmdhd_mfg.bin_b1 \
    "$FW_DIR"/bcmdhd_clm.blob \
    "$FW_DIR"/nvram.txt_* \
    "$DEVICE:/tmp/wlanfw/"
ssh -i "$SSH_KEY" -o StrictHostKeyChecking=no "$DEVICE" \
    'mkdir -p /lib/firmware && mv /tmp/wlanfw/* /lib/firmware/ && sync && ls -la /lib/firmware/ | grep -E "bcmdhd|nvram"'
echo "==> Done. Firmware loads on next WLAN bring-up (reboot or rebind)."
