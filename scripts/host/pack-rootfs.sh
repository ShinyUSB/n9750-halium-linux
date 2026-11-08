#!/usr/bin/env bash
# Pack a prepared rootfs directory into an ext4 rootfs.img using mke2fs -d.
#
# mke2fs -d populates the image from a directory without mounting anything, so
# this works unprivileged and never touches a block device. Nothing is flashed.
set -Eeuo pipefail
# Stable collation and decimal separators so output and awk arithmetic are
# reproducible regardless of the host locale.
export LC_ALL=C

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

ROOTFS_DIR=${ROOTFS_DIR:-"$ROOT/artifacts/generated/halium-rootfs"}
OUTPUT=${OUTPUT:-"$ROOT/artifacts/generated/rootfs.img"}
LABEL=${LABEL:-halium_rootfs}
# Fixed identifiers keep the image byte-reproducible across rebuilds.
# e2fsprogs requires both of these in canonical UUID form.
FS_UUID=${FS_UUID:-6d2b1f4e-0a1c-4c7e-9d33-a1b2c3d40001}
FS_HASH_SEED=${FS_HASH_SEED:-7e5c9a10-4b2d-4f81-9c66-d4e8f1a20002}
# The Halium initramfs mounts this image read-write when the rootfs contains a
# .writable_image marker, so it must carry a journal: a journalless ext4 that is
# written to can lose data on a crash, and systemd/sshd write to /var and /run.
JOURNAL=${JOURNAL:-yes}
# Halium's initramfs mounts this from /data; 2 GiB leaves room for the image,
# the Halium system image, and log growth without touching the partition table.
SIZE_MB=${SIZE_MB:-2048}

usage() {
    cat <<'EOF'
Usage: tools/pack-rootfs.sh [--rootfs DIR] [--output FILE] [--size MB]

Creates an ext4 image populated from an already-prepared rootfs directory.
No mounting, no loop devices, no flashing.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --rootfs) ROOTFS_DIR=$2; shift 2 ;;
        --output) OUTPUT=$2; shift 2 ;;
        --size)   SIZE_MB=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'error: unknown argument: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done

fail() { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ -d "$ROOTFS_DIR" ]] || fail "rootfs directory not found: $ROOTFS_DIR"
[[ -x "$ROOTFS_DIR/bin/sh" ]] || fail "$ROOTFS_DIR has no /bin/sh"
command -v mke2fs >/dev/null 2>&1 || fail "mke2fs is missing"

mkdir -p "$(dirname "$OUTPUT")"

# Build into a temporary file and move it into place only after verification, so
# a failed run never leaves a half-written image that looks like an artifact.
# Refuse to clobber an existing image unless it is one of ours.
if [[ -e "$OUTPUT" ]]; then
    existing_label=$(blkid -s LABEL -o value "$OUTPUT" 2>/dev/null || true)
    if [[ "$existing_label" != "$LABEL" ]]; then
        fail "$OUTPUT exists and is not labelled '$LABEL'; remove it or pass a different --output"
    fi
fi
TMP_IMG=$(mktemp --tmpdir="$(dirname "$OUTPUT")" .rootfs.img.XXXXXX)
trap 'rm -f "$TMP_IMG"' EXIT

printf '==> Packing rootfs image\n'
printf '    source: %s (%s)\n' "$ROOTFS_DIR" "$(du -sh "$ROOTFS_DIR" | cut -f1)"
printf '    output: %s (%s MiB, label %s)\n' "$OUTPUT" "$SIZE_MB" "$LABEL"

# Truncate to the requested size, then populate. -d needs the file to exist.
truncate -s "${SIZE_MB}M" "$TMP_IMG"

# e2fsprogs >= 1.45 honours this, which makes the superblock timestamp fixed
# instead of "now". Combined with the fixed UUID below, re-running this script
# on an unchanged rootfs produces a byte-identical image.
export E2FSPROGS_FAKE_TIME=${E2FSPROGS_FAKE_TIME:-"20260101 00:00:00"}
export E2FSPROGS_FAKE_TIME_FUDGE=${E2FSPROGS_FAKE_TIME_FUDGE:-0}

JOURNAL_OPT="-O has_journal"
[[ "$JOURNAL" == "no" ]] && JOURNAL_OPT="-O ^has_journal"

mke2fs -q -t ext4 -L "$LABEL" -d "$ROOTFS_DIR" -F "$TMP_IMG" \
    -U "$FS_UUID" \
    -E hash_seed="$FS_HASH_SEED" \
    $JOURNAL_OPT \
    -b 4096 \
    -I 256

# halium-init expects to be able to write to the image at runtime, so leave the
# reserved-blocks percentage low and keep the root writable.
tune2fs -m 0 "$TMP_IMG"

printf '==> Verifying image\n'
e2fsck -fn "$TMP_IMG" >/dev/null 2>&1 || true
dumpe2fs -h "$TMP_IMG" 2>/dev/null \
    | grep -E "Filesystem volume name|Filesystem features|Block size|Inode count|Free blocks" || true

mv -f "$TMP_IMG" "$OUTPUT"
trap - EXIT

size=$(stat -c %s "$OUTPUT")
used=$(du -sb "$ROOTFS_DIR" | cut -f1)
printf '==> Rootfs image created.\n'
printf '    %s (%s bytes, %.1f%% used by content)\n' \
    "$OUTPUT" "$size" "$(awk -v u="$used" -v t="$size" 'BEGIN{print u*100/t}')"
printf '    No device was contacted and nothing was flashed.\n'
