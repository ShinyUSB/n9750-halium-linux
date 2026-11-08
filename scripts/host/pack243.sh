#!/usr/bin/env bash
# Pack a Samsung d2q Halium boot image the way images 242/243 were actually
# built. Validated by reproducing image 242 byte-for-byte apart from the
# kernel payload.
#
# WHY NOT tools/package-boot.sh defaults:
#   1. /usr/bin/mkbootimg adds --base 0x10000000, which moves kernel_addr from
#      0x8000 to 0x10008000. Every d2q image uses base 0.
#   2. The Halium mkbootimg
#      (n9750-halium/out/host/linux-x86/bin/mkbootimg) is a different binary
#      from /usr/bin/mkbootimg and is the one that produced these images.
#   3. /usr/bin/mkbootimg also truncates the cmdline into extra_cmdline,
#      which is NOT what 242 does (see CMDLINE note below).
#
# CMDLINE NOTE -- a real trap in the 242 build:
#   242's cmdline is 535 chars but the header cmdline field is 512. The
#   original builder did not bound-check and let it OVERFLOW 23 bytes into the
#   image `id` field at offset 576. Consequence: the trailing
#   "d=1 a6xx.uche_gbif_rw=1" was NEVER delivered to the kernel -- the
#   bootloader only reads the 512-byte field, and extra_cmdline was all zeros.
#   The kernel-visible cmdline therefore ended mid-token at "a6xx.send_ac",
#   exactly as RESUME.md warns ("Bootloader truncates cmdline ~512B -> a6xx
#   flags must LEAD").
#
#   This script therefore truncates to exactly 512 chars, reproducing 242's
#   effective kernel-visible cmdline so that an image differs from its
#   predecessor ONLY in the kernel.
#
# Never flashes. Writes a file and verifies the header against a reference.
set -Eeuo pipefail

MK=${MK:-/home/usb/n9750-halium/out/host/linux-x86/bin/mkbootimg}
RAMDISK=${RAMDISK:-/tmp/opencode/ramdisk242.bin}
CMDLINE_FILE=${CMDLINE_FILE:-/tmp/opencode/cmdline243.txt}
KERNEL=${KERNEL:-/home/usb/mnt-sda/a6xx-build/out-gpu/arch/arm64/boot/Image-dtb}
OUTPUT=${OUTPUT:-}
REFERENCE=${REFERENCE:-/home/usb/artifacts/flash/242-halium-boot-a6xx-llccb2.img}

[[ -x "$MK"     ]] || { echo "missing Halium mkbootimg: $MK" >&2; exit 1; }
[[ -f "$RAMDISK"        ]] || { echo "missing ramdisk: $RAMDISK" >&2; exit 1; }
[[ -f "$CMDLINE_FILE"   ]] || { echo "missing cmdline: $CMDLINE_FILE" >&2; exit 1; }
[[ -f "$KERNEL"         ]] || { echo "missing kernel: $KERNEL" >&2; exit 1; }
[[ -n "$OUTPUT"         ]] || { echo "set OUTPUT=" >&2; exit 1; }

CMD=$(head -c 512 "$CMDLINE_FILE")
printf '==> cmdline %d bytes (kernel-visible)\n' "${#CMD}"

"$MK" \
    --kernel      "$KERNEL" \
    --ramdisk     "$RAMDISK" \
    --kernel_offset   0x00008000 \
    --ramdisk_offset  0x02000000 \
    --second_offset   0x00f00000 \
    --tags_offset     0x01e00000 \
    --cmdline     "$CMD" \
    --board       FRPSC27A006 \
    --pagesize    4096 \
    --header_version 1 \
    --base        0x00000000 \
    --output      "$OUTPUT"

printf '==> built: %s\n' "$OUTPUT"
sha256sum "$OUTPUT"

if [[ -f "$REFERENCE" ]]; then
    python3 - "$OUTPUT" "$REFERENCE" <<'PY'
import sys
a = open(sys.argv[1], 'rb').read(2048)
b = open(sys.argv[2], 'rb').read(2048)
diff = [i for i in range(2048) if a[i] != b[i]]
outside = [i for i in diff if not (576 <= i < 608)]
print("==> header bytes differing from reference: %d" % len(diff))
if outside:
    print("!!! UNEXPECTED DIFFERENCES OUTSIDE THE id FIELD:", outside)
    print("!!! This image is NOT a single-variable change. Do not flash.")
    sys.exit(1)
print("==> OK: header identical to reference except the sha1 id field.")
print("==> The ONLY functional difference from the reference is the kernel.")
PY
fi