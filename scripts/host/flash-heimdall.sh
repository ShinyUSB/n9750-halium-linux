#!/usr/bin/env bash
# Safety wrapper for the explicitly reviewed Samsung BOOT-only operation.
set -Eeuo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
IMAGE=${IMAGE:-"$ROOT/artifacts/flash/01-halium-boot-halium-kernel.img"}
STOCK_BOOT=${STOCK_BOOT:-"$ROOT/ShinyKernelREVIEWED/Kernel/stock/boot.img"}
HEIMDALL=${HEIMDALL:-heimdall}
EXECUTE=0

usage() {
    cat <<'EOF'
Usage: tools/flash-heimdall.sh [--execute] [--image FILE]

Default behavior is dry-run: it prints the exact BOOT-only command and exits.
--execute additionally requires I_HAVE_A_RECOVERY_PATH=YES and a recent
recovery image. It still refuses PIT, repartition, erase, and userdata flags.

Staged images (see artifacts/flash/MANIFEST.md):
  02-halium-boot-baseline-kernel.img   flash this first, to isolate the kernel
                                       and boot chain from the Halium userspace
  01-halium-boot-halium-kernel.img     the primary image

The rootfs image is NOT flashed by this script. Heimdall cannot place a file
inside the userdata partition, and writing it is the only destructive step.

Recovery (restores the preserved stock boot image):
  tools/flash-heimdall.sh --image ShinyKernelREVIEWED/Kernel/stock/boot.img --execute
EOF
}

while (($#)); do
    case "$1" in
        --execute) EXECUTE=1; shift ;;
        --image) IMAGE=${2:-}; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'error: unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done

[[ -f "$IMAGE" ]] || { printf 'error: image not found: %s\n' "$IMAGE" >&2; exit 1; }

printf 'Planned operation: flash only the BOOT partition.\n'
printf 'Image: %s\n' "$IMAGE"
printf 'SHA256: %s\n' "$(sha256sum "$IMAGE" | cut -d' ' -f1)"
printf 'Command: %s flash --BOOT %q\n' "$HEIMDALL" "$IMAGE"
printf 'No PIT, repartitioning, wipe, or userdata operation is permitted.\n'

if [[ -f "$STOCK_BOOT" ]]; then
    printf 'Recovery image available: %s\n' "$STOCK_BOOT"
    printf '  heimdall flash --BOOT %q\n' "$STOCK_BOOT"
fi

if ((EXECUTE == 0)); then
    printf 'Dry-run only. Re-run with --execute only after manual review.\n'
    exit 0
fi

command -v "$HEIMDALL" >/dev/null 2>&1 || {
    printf 'error: Heimdall is not installed: %s\n' "$HEIMDALL" >&2
    printf 'Build it from source: git clone https://github.com/Benjamin-Dobell/Heimdall\n' >&2
    exit 1
}

[[ "${I_HAVE_A_RECOVERY_PATH:-}" == YES ]] || {
    printf 'error: set I_HAVE_A_RECOVERY_PATH=YES to acknowledge recovery risk\n' >&2
    exit 1
}

printf 'Executing the explicitly requested BOOT-only flash now.\n'
exec "$HEIMDALL" flash --BOOT "$IMAGE"
