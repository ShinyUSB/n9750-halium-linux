#!/usr/bin/env bash
# Prepare a host-side Halium rootfs directory without contacting or flashing a phone.
set -Eeuo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
HALIUM_INSTALL=${HALIUM_INSTALL:-"$ROOT/halium-install/halium-install"}
ROOTFS_TAR=${ROOTFS_TAR:-}
SYSTEM_IMG=${SYSTEM_IMG:-}
OUTPUT_DIR=${OUTPUT_DIR:-"$ROOT/artifacts/generated/halium-rootfs"}
ROOTFS_RELEASE=${ROOTFS_RELEASE:-halium}

usage() {
    cat <<'EOF'
Usage: ROOTFS_TAR=... SYSTEM_IMG=... tools/build-rootfs.sh

The script calls halium-install in --test-mode --mode dir. It extracts and
prepares images on the host only; it never invokes adb, fastboot, Odin, or
Heimdall. The input rootfs must be an ARM64 archive and system.img must be a
sparse or raw Android system image.
EOF
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    usage
    exit 0
fi

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

[[ -n "$ROOTFS_TAR" ]] || fail "set ROOTFS_TAR to an ARM64 Halium/Debian rootfs archive"
[[ -n "$SYSTEM_IMG" ]] || fail "set SYSTEM_IMG to the Android system image"
[[ -f "$ROOTFS_TAR" ]] || fail "rootfs archive not found: $ROOTFS_TAR"
[[ -f "$SYSTEM_IMG" ]] || fail "system image not found: $SYSTEM_IMG"
[[ -x "$HALIUM_INSTALL" ]] || fail "halium-install is not executable: $HALIUM_INSTALL"

for cmd in tar qemu-aarch64-static qemu-arm-static simg2img; do
    command -v "$cmd" >/dev/null 2>&1 || fail "required host command is missing: $cmd"
done

mkdir -p "$OUTPUT_DIR"
ROOTFS_DIR="$OUTPUT_DIR/rootfs"
IMAGE_DIR="$OUTPUT_DIR/images"
mkdir -p "$ROOTFS_DIR" "$IMAGE_DIR"

printf '==> Preparing rootfs on the host (no device access)\n'
printf '    rootfs: %s\n' "$ROOTFS_TAR"
printf '    system: %s\n' "$SYSTEM_IMG"
printf '    output: %s\n' "$OUTPUT_DIR"

ROOTFS_DIR="$ROOTFS_DIR" IMAGE_DIR="$IMAGE_DIR" \
    "$HALIUM_INSTALL" --mode dir --test-mode -p "$ROOTFS_RELEASE" \
    "$ROOTFS_TAR" "$SYSTEM_IMG"

[[ -f "$ROOTFS_DIR/var/lib/lxc/android/system.img" ]] \
    || fail "halium-install did not produce the injected system image"
[[ -x "$ROOTFS_DIR/bin/sh" ]] || fail "prepared rootfs has no executable /bin/sh"

printf '==> Rootfs preparation verified; no device operation was performed.\n'
printf '    %s\n' "$ROOTFS_DIR"
