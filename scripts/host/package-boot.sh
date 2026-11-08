#!/usr/bin/env bash
# Package a Samsung d2q Halium boot image locally. This script never flashes.
set -Eeuo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
KERNEL_DIR=${KERNEL_DIR:-"$ROOT/ShinyKernel/Kernel"}
KERNEL_IMAGE=${KERNEL_IMAGE:-"$ROOT/artifacts/known-good/shinykernel-baseline/Image-dtb"}
RAMDISK=${RAMDISK:-}
OUTPUT=${OUTPUT:-"$ROOT/artifacts/generated/halium-boot.img"}
MKBOOTIMG=${MKBOOTIMG:-}
BOARD_NAME=${BOARD_NAME:-FRPSC27A006}
PAGE_SIZE=${PAGE_SIZE:-4096}
KERNEL_BASE=${KERNEL_BASE:-0x00000000}
KERNEL_OFFSET=${KERNEL_OFFSET:-0x00008000}
RAMDISK_OFFSET=${RAMDISK_OFFSET:-0x02000000}
TAGS_OFFSET=${TAGS_OFFSET:-0x01E00000}
SECOND_OFFSET=${SECOND_OFFSET:-0x00F00000}
HEADER_VERSION=${HEADER_VERSION:-1}
OS_VERSION=${OS_VERSION-}
OS_PATCH_LEVEL=${OS_PATCH_LEVEL-}
KERNEL_CMDLINE=${KERNEL_CMDLINE:-"console=null androidboot.hardware=qcom androidboot.console=ttyMSM0 androidboot.memcg=1 lpm_levels.sleep_disabled=1 video=vfb:640x400,bpp=32,memsize=3072000 msm_rtb.filter=0x237 service_locator.enable=1 swiotlb=2048 androidboot.usbcontroller=a600000.dwc3 firmware_class.path=/vendor/firmware_mnt/image loop.max_part=7"}

usage() {
    cat <<'EOF'
Usage: tools/package-boot.sh --ramdisk initrd.img [options]

Required:
  --ramdisk FILE       Halium ARM64 initramfs (initrd.img-touch-arm64)

Options:
  --kernel FILE        Image-dtb (default: preserved baseline artifact)
  --output FILE        Output boot image (default: artifacts/generated/halium-boot.img)
  --mkbootimg FILE     mkbootimg executable
  --board NAME         Samsung board name (default: FRPSC27A006)
  --cmdline STRING     Override the verified stock kernel command line
  -h, --help           Show this help

Environment:
  ALLOW_LEGACY_MKBOOTIMG=1  Allow the kernel-tree fallback for a smoke test

The script only writes a local image and verifies its header. It never invokes
Heimdall, Odin, fastboot, or any flashing operation.
EOF
}

while (($#)); do
    case "$1" in
        --ramdisk) RAMDISK=${2:-}; shift 2 ;;
        --kernel) KERNEL_IMAGE=${2:-}; shift 2 ;;
        --output) OUTPUT=${2:-}; shift 2 ;;
        --mkbootimg) MKBOOTIMG=${2:-}; shift 2 ;;
        --board) BOARD_NAME=${2:-}; shift 2 ;;
        --cmdline) KERNEL_CMDLINE=${2:-}; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'error: unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

[[ -n "$RAMDISK" ]] || { usage >&2; fail '--ramdisk is required'; }
[[ -f "$KERNEL_IMAGE" ]] || fail "kernel image not found: $KERNEL_IMAGE"
[[ -f "$RAMDISK" ]] || fail "ramdisk not found: $RAMDISK"

MKBOOTIMG_SOURCE=explicit
if [[ -z "$MKBOOTIMG" ]]; then
    for candidate in \
        "$ROOT/n9750-halium/out/host/linux-x86/bin/mkbootimg" \
        "$ROOT/n9750-halium/out/host/linux-x86_64/bin/mkbootimg"; do
        if [[ -x "$candidate" ]]; then
            MKBOOTIMG=$candidate
            MKBOOTIMG_SOURCE=halium
            break
        fi
    done
fi
if [[ -z "$MKBOOTIMG" && -x "$KERNEL_DIR/tools/mkbootimg" && "${ALLOW_LEGACY_MKBOOTIMG:-0}" == 1 ]]; then
    MKBOOTIMG="$KERNEL_DIR/tools/mkbootimg"
    MKBOOTIMG_SOURCE=legacy-kernel
fi
[[ -n "$MKBOOTIMG" && -x "$MKBOOTIMG" ]] || fail "Halium mkbootimg was not found; pass --mkbootimg"
if [[ "$MKBOOTIMG_SOURCE" == legacy-kernel ]]; then
    printf 'warning: using the legacy kernel mkbootimg; verify OS header fields before any flash\n' >&2
fi

mkdir -p "$(dirname -- "$OUTPUT")"
args=(
    --kernel "$KERNEL_IMAGE"
    --ramdisk "$RAMDISK"
    --output "$OUTPUT"
    --base "$KERNEL_BASE"
    --kernel_offset "$KERNEL_OFFSET"
    --ramdisk_offset "$RAMDISK_OFFSET"
    --second_offset "$SECOND_OFFSET"
    --tags_offset "$TAGS_OFFSET"
    --pagesize "$PAGE_SIZE"
    --header_version "$HEADER_VERSION"
    --board "$BOARD_NAME"
    --cmdline "$KERNEL_CMDLINE"
)
if [[ -n "$OS_VERSION" ]]; then
    args+=(--os_version "$OS_VERSION")
fi
if [[ -n "$OS_PATCH_LEVEL" ]]; then
    args+=(--os_patch_level "$OS_PATCH_LEVEL")
fi

printf '==> Packaging %s\n' "$OUTPUT"
printf '    kernel:  %s\n' "$KERNEL_IMAGE"
printf '    ramdisk: %s\n' "$RAMDISK"
printf '    board:   %s, header v%s, page %s\n' "$BOARD_NAME" "$HEADER_VERSION" "$PAGE_SIZE"
"$MKBOOTIMG" "${args[@]}"

python3 - "$OUTPUT" <<'PY'
import struct
import sys

path = sys.argv[1]
data = open(path, 'rb').read(2048)
if data[:8] != b'ANDROID!':
    raise SystemExit('error: output does not have an Android boot header')
(kernel_size, kernel_addr, ramdisk_size, ramdisk_addr, second_size,
 second_addr, tags_addr, page_size, header_version, os_version) = struct.unpack_from('<10I', data, 8)
print(f'    header: kernel={kernel_size} ramdisk={ramdisk_size} page={page_size} '
      f'header_v={header_version} kernel_addr=0x{kernel_addr:x} '
      f'ramdisk_addr=0x{ramdisk_addr:x} tags=0x{tags_addr:x}')
if ramdisk_size == 0:
    raise SystemExit('error: packaged image has an empty ramdisk')
PY

printf '==> Local image verified; no flashing was performed.\n'
printf '    %s\n' "$OUTPUT"
