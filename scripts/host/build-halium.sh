#!/usr/bin/env bash
# Reproducible, non-destructive build entry point for the SM-N9750/d2q port.
set -Eeuo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

KERNEL_DIR=${KERNEL_DIR:-"$ROOT/ShinyKernel/Kernel"}
HALIUM_DIR=${HALIUM_DIR:-"$ROOT/n9750-halium"}
ARTIFACT_DIR=${ARTIFACT_DIR:-"$ROOT/artifacts"}
KNOWN_GOOD_DIR=${KNOWN_GOOD_DIR:-"$ARTIFACT_DIR/known-good/shinykernel-baseline"}
STOCK_BOOT=${STOCK_BOOT:-"$ROOT/ShinyKernelREVIEWED/Kernel/stock/boot.img"}
DEVICE_IP=${DEVICE_IP:-10.15.19.82}
BUILD_JOBS=${BUILD_JOBS:-2}
KERNEL_OUT=${KERNEL_OUT:-"$KERNEL_DIR/out"}
KERNEL_LOG=${KERNEL_LOG:-"$KERNEL_DIR/build_log.txt"}
HALIUM_LUNCH_TARGET=${HALIUM_LUNCH_TARGET:-lineage_halium_d2q-userdebug}
HALIUM_INSTALL=${HALIUM_INSTALL:-"$ROOT/halium-install/halium-install"}
# The ARM64 Debian rootfs is the supported Halium pairing. The published Halium
# reference rootfs is 32-bit armhf; it is kept only for comparison and is not the
# deliverable. See PORTING-STATUS.md.
ROOTFS_DIR=${ROOTFS_DIR:-"$ARTIFACT_DIR/generated/halium-rootfs-arm64"}
ROOTFS_IMG=${ROOTFS_IMG:-"$ARTIFACT_DIR/generated/rootfs-arm64.img"}
KEY_DIR=${KEY_DIR:-"$ARTIFACT_DIR/keys"}
HOST_IP=${HOST_IP:-10.15.19.1}

MODE=all
FLASH_REQUESTED=0

usage() {
    cat <<'EOF'
Usage: tools/build-halium.sh [options]

Options:
  (default)         Same as --all
  --kernel-only     Build and verify the ShinyKernel d2q baseline kernel
  --halium-kernel   Build the headless Halium-profile kernel (out-halium)
  --boot-only       Build the Halium boot image only
  --rootfs-only     Build, verify and pack the headless ARM64 rootfs
  --halium-install  Run the full halium-install path (needs SYSTEM_IMG)
  --all             Baseline kernel -> Halium kernel -> boot image -> rootfs image
  --flash           Print the reviewed Heimdall command; never flashes implicitly

Environment:
  BUILD_JOBS             Parallel kernel jobs (default: 2 on this host)
  KERNEL_OUT              Baseline kernel output (default: ShinyKernel/Kernel/out)
  HALIUM_LUNCH_TARGET     Breakfast target used by --boot-only
  DEVICE_IP               Documented device address (default: 10.15.19.82)
  HOST_IP                 Documented host address (default: 10.15.19.1)
  ROOTFS_TAR              Halium reference rootfs (only for --halium-install)
  ROOTFS_DIR              Extracted rootfs working directory
  ROOTFS_IMG              Output ext4 rootfs image
  SYSTEM_IMG              Android system image; required only by --halium-install
EOF
}

while (($#)); do
    case "$1" in
        --kernel-only) MODE=kernel ;;
        --halium-kernel) MODE=halium-kernel ;;
        --boot-only) MODE=boot ;;
        --rootfs-only) MODE=rootfs ;;
        --halium-install) MODE=halium-install ;;
        --all) MODE=all ;;
        --flash) FLASH_REQUESTED=1 ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'error: unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

require_file() {
    [[ -f "$1" ]] || fail "required file is missing: $1"
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || fail "required command is missing: $1"
}

verify_kernel_inputs() {
    require_file "$KERNEL_DIR/build.txt"
    require_file "$KERNEL_DIR/arch/arm64/configs/d2q_chn_hk_defconfig"
    require_file "$KERNEL_DIR/toolchains/Snapdragon-LLVM/bin/clang"
    require_file "$KERNEL_DIR/toolchains/gcc-aarch64/bin/aarch64-linux-android-gcc"
    require_file "$KERNEL_DIR/tools/dtc"
    require_cmd distrobox
    require_cmd podman
    distrobox list | awk -F'|' '$2 ~ /kernel-build/ { found=1 } END { exit(found ? 0 : 1) }' \
        || fail "the kernel-build distrobox does not exist"
}

build_kernel() {
    verify_kernel_inputs
    mkdir -p "$ARTIFACT_DIR" "$KNOWN_GOOD_DIR"

    printf '==> Building ShinyKernel in the Ubuntu 18.04 kernel-build container\n'
    printf '    jobs: %s (the source build.txt uses -j8; lower parallelism avoids host OOM)\n' "$BUILD_JOBS"

    distrobox enter kernel-build --no-tty -- bash -s -- \
        "$KERNEL_DIR" "$KERNEL_OUT" "$KERNEL_LOG" "$BUILD_JOBS" <<'CONTAINER'
set -Eeuo pipefail
kernel_dir=$1
kernel_out=$2
kernel_log=$3
build_jobs=$4

cd "$kernel_dir"
mkdir -p "$kernel_out"

export ARCH=arm64
export SUBARCH=arm64
export CROSS_COMPILE="$kernel_dir/toolchains/gcc-aarch64/bin/aarch64-linux-android-"
export REAL_CC="$kernel_dir/toolchains/Snapdragon-LLVM/bin/clang"
export KCONFIG_CONFIG="$kernel_out/.config"

make -C "$kernel_dir" O="$kernel_out" \
    DTC_EXT="$kernel_dir/tools/dtc" \
    CONFIG_BUILD_ARM64_DT_OVERLAY=y \
    CLANG_TRIPLE=aarch64-linux-gnu- \
    CROSS_COMPILE="$CROSS_COMPILE" \
    d2q_chn_hk_defconfig

make -C "$kernel_dir" O="$kernel_out" \
    ARCH=arm64 \
    DTC_EXT="$kernel_dir/tools/dtc" \
    CONFIG_BUILD_ARM64_DT_OVERLAY=y \
    CLANG_TRIPLE=aarch64-linux-gnu- \
    CC="$REAL_CC" \
    REAL_CC="$REAL_CC" \
    CROSS_COMPILE="$CROSS_COMPILE" \
    KCFLAGS="--target=aarch64-linux-gnu -march=armv8.2-a+crypto -pipe" \
    -j"$build_jobs" 2>&1 | tee "$kernel_log"
CONTAINER

    require_file "$KERNEL_OUT/arch/arm64/boot/Image"
    require_file "$KERNEL_OUT/arch/arm64/boot/Image-dtb"
    require_file "$KERNEL_OUT/arch/arm64/boot/dts/qcom/sm8150.dtb"
    require_file "$KERNEL_OUT/arch/arm64/boot/dts/qcom/sm8150-v2.dtb"

    local dtbo_count
    dtbo_count=$(find "$KERNEL_OUT/arch/arm64/boot/dts/samsung/davinci" -name '*.dtbo' -type f | wc -l)
    [[ "$dtbo_count" -eq 15 ]] || fail "expected 15 d2q DTBO files, found $dtbo_count"

    mkdir -p "$ARTIFACT_DIR/generated/kernel"
    cp -a "$KERNEL_OUT/arch/arm64/boot/Image" "$ARTIFACT_DIR/generated/kernel/Image"
    cp -a "$KERNEL_OUT/arch/arm64/boot/Image-dtb" "$ARTIFACT_DIR/generated/kernel/Image-dtb"
    cp -a "$KERNEL_OUT/.config" "$ARTIFACT_DIR/generated/kernel/config"
    cp -a "$KERNEL_LOG" "$ARTIFACT_DIR/generated/kernel/build.log"

    printf '==> Kernel artifacts:\n'
    sha256sum \
        "$ARTIFACT_DIR/generated/kernel/Image" \
        "$ARTIFACT_DIR/generated/kernel/Image-dtb" \
        "$ARTIFACT_DIR/generated/kernel/config"
}

verify_halium_tree() {
    require_file "$HALIUM_DIR/build/envsetup.sh"
    [[ -d "$HALIUM_DIR/bootable/halium-boot" ]] \
        || fail "Halium boot tooling is missing: $HALIUM_DIR/bootable/halium-boot"
    [[ -d "$HALIUM_DIR/device/halium/halium_arm64" ]] \
        || fail "generic Halium ARM64 device tree is missing"
    [[ -f "$HALIUM_DIR/device/samsung/d2q/BoardConfig.mk" ]] \
        || fail "d2q Halium device overlay is missing"
    [[ -f "$HALIUM_DIR/device/samsung/d2q/prebuilt/Image-dtb" ]] \
        || fail "d2q prebuilt Image-dtb is missing"
}

build_boot() {
    verify_halium_tree
    printf '==> Building Halium boot image for %s\n' "$HALIUM_LUNCH_TARGET"
    # envsetup.sh and lunch are not nounset-clean: they reference unset
    # variables like ZSH_VERSION and will abort under `set -u`.
    set +u
    (
        set +u
        cd "$HALIUM_DIR"
        source build/envsetup.sh
        lunch "$HALIUM_LUNCH_TARGET"
        # mkbootimg is invoked through $(MKBOOTIMG) by bootable/halium-boot;
        # it must exist before the recipe runs or the PATH-tools allowlist
        # rejects the bare name.
        m mkbootimg
        mka halium-boot
    )
    set -u
    local boot_image
    boot_image=$(find "$HALIUM_DIR/out/target/product" -type f -name 'halium-boot.img' -print -quit)
    [[ -n "$boot_image" ]] || fail "mka halium-boot did not produce halium-boot.img"
    mkdir -p "$ARTIFACT_DIR/generated/halium"
    cp -a "$boot_image" "$ARTIFACT_DIR/generated/halium/halium-boot.img"
    printf '==> Halium boot artifact: %s\n' "$ARTIFACT_DIR/generated/halium/halium-boot.img"
    sha256sum "$ARTIFACT_DIR/generated/halium/halium-boot.img"
    verify_boot_image "$ARTIFACT_DIR/generated/halium/halium-boot.img"
}

# Confirm the packed boot image still carries the geometry the Samsung
# bootloader expects, taken from the preserved stock boot image.
verify_boot_image() {
    local img=$1
    require_file "$img"
    local unpacker="$HALIUM_DIR/system/core/mkbootimg/unpack_bootimg.py"
    require_file "$unpacker"
    local unpack
    unpack=$(mktemp -d)
    # unpack_bootimg.py prints the header to stdout and extracts the payloads.
    local hdr
    hdr=$(tr -d '\000' < <("$unpacker" --boot_img "$img" --out "$unpack"))

    boot_expect() {
        local field=$1 want=$2 got
        got=$(printf '%s\n' "$hdr" | sed -n "s/^${field}: //p" | head -n 1)
        [[ "$got" == "$want" ]] \
            || fail "boot image '$field' is '$got', expected '$want'"
        printf '    ok  %-32s %s\n' "$field" "$got"
    }

    boot_expect boot_magic "ANDROID!"
    boot_expect "kernel load address" "0x8000"
    boot_expect "ramdisk load address" "0x2000000"
    boot_expect "second bootloader load address" "0xf00000"
    boot_expect "kernel tags load address" "0x1e00000"
    boot_expect "page size" "4096"
    boot_expect "boot image header version" "1"

    local ksize rsize
    ksize=$(printf '%s\n' "$hdr" | sed -n 's/^kernel_size: //p' | head -n 1)
    rsize=$(printf '%s\n' "$hdr" | sed -n 's/^ramdisk size: //p' | head -n 1)
    [[ -n "$ksize" && "$ksize" -gt 0 ]] || fail "boot image has an empty kernel"
    [[ -n "$rsize" && "$rsize" -gt 0 ]] || fail "boot image has an empty ramdisk"
    printf '    ok  %-32s %s\n' "kernel_size" "$ksize"
    printf '    ok  %-32s %s\n' "ramdisk_size" "$rsize"

    # The kernel payload must be the exact Image-dtb we selected, not something
    # left over from an earlier build.
    local prebuilt="$HALIUM_DIR/device/samsung/d2q/prebuilt/Image-dtb"
    if [[ -f "$prebuilt" ]]; then
        cmp -s "$unpack/kernel" "$prebuilt" \
            || fail "boot image kernel does not match $prebuilt"
        printf '    ok  %-32s %s\n' "kernel payload" "matches prebuilt/Image-dtb"
    fi

    # The ramdisk must be the Halium initramfs, not an Android one.
    if [[ "$(od -An -tx1 -N2 "$unpack/ramdisk" | tr -d ' \n')" == "1f8b" ]]; then
        printf '    ok  %-32s %s\n' "ramdisk payload" "gzip (Halium initramfs)"
    else
        fail "ramdisk payload is not gzip; the Halium initramfs was not used"
    fi

    rm -rf "$unpack"
    printf '==> Boot image geometry verified against the stock Samsung layout.\n'
}

# Headless ARM64 rootfs: build it, verify it, then pack an ext4 image.
# No device access at any point, and no Android system image is required.
prepare_rootfs() {
    printf '==> Building the headless ARM64 rootfs\n'
    ROOTFS_DIR="$ROOTFS_DIR" KEY_DIR="$KEY_DIR" DEVICE_IP="$DEVICE_IP" HOST_IP="$HOST_IP" \
        "$ROOT/tools/build-rootfs-arm64.sh"

    printf '==> Verifying the ARM64 rootfs\n'
    ROOTFS_DIR="$ROOTFS_DIR" "$ROOT/tools/verify-rootfs.sh"

    printf '==> Packing the rootfs image\n'
    ROOTFS_DIR="$ROOTFS_DIR" OUTPUT="$ROOTFS_IMG" \
        "$ROOT/tools/pack-rootfs.sh"

    require_file "$ROOTFS_IMG"
    printf '==> Rootfs artifact: %s\n' "$ROOTFS_IMG"
    sha256sum "$ROOTFS_IMG"
}

# Full Halium installation with an Android system image. This is NOT required
# for the headless target: the Halium initramfs tolerates a missing system.img
# and still switches into the native rootfs. See PORTING-STATUS.md section K.
halium_install() {
    [[ -n "${SYSTEM_IMG:-}" ]] \
        || fail "--halium-install requires SYSTEM_IMG (an arm64 Android system image)
NOTE: the headless target does not need this. Run --rootfs-only instead."
    [[ -n "${ROOTFS_TAR:-}" ]] \
        || fail "--halium-install requires ROOTFS_TAR (the Halium reference rootfs)"
    require_file "$ROOTFS_TAR"
    require_file "$SYSTEM_IMG"
    [[ -x "$HALIUM_INSTALL" ]] || fail "halium-install is not executable: $HALIUM_INSTALL"
    printf '==> Running halium-install in --test-mode --mode dir; no device access.\n'
    ROOTFS_TAR="$ROOTFS_TAR" SYSTEM_IMG="$SYSTEM_IMG" HALIUM_INSTALL="$HALIUM_INSTALL" \
        "$ROOT/tools/build-rootfs.sh"
}

# Install the headless-profile kernel as the d2q prebuilt that halium-boot
# consumes. The baseline output tree is left untouched; the previous prebuilt
# is kept as a fallback next to the new one.
install_halium_kernel() {
    local halium_out="$KERNEL_DIR/out-halium"
    local prebuilt_dir="$HALIUM_DIR/device/samsung/d2q/prebuilt"
    require_file "$halium_out/arch/arm64/boot/Image-dtb"
    require_file "$halium_out/.config"

    mkdir -p "$prebuilt_dir"
    if [[ -f "$prebuilt_dir/Image-dtb" ]]; then
        cp -a "$prebuilt_dir/Image-dtb" "$prebuilt_dir/Image-dtb.baseline"
        printf '==> Previous prebuilt kernel kept at %s\n' "$prebuilt_dir/Image-dtb.baseline"
    fi
    cp -a "$halium_out/arch/arm64/boot/Image-dtb" "$prebuilt_dir/Image-dtb"
    cp -a "$halium_out/.config" "$prebuilt_dir/halium-kernel.config"

    printf '==> Installed Halium-profile kernel\n'
    sha256sum "$prebuilt_dir/Image-dtb" "$prebuilt_dir/halium-kernel.config"
    "$ROOT/tools/check-kernel.sh" "$prebuilt_dir/halium-kernel.config" \
        || fail "headless kernel config check failed"
}

print_next_steps() {
    cat <<EOF

==> Artifact summary
    boot image : $ARTIFACT_DIR/generated/halium/halium-boot.img
    rootfs img : $ROOTFS_IMG
    client key : $KEY_DIR/d2q_rsa
    host keys  : $KEY_DIR/ssh_host_ed25519_key (the device SSH identity)

==> Flashing (NOT performed by this script; review every line)
    partition : BOOT only
    image     : $ARTIFACT_DIR/generated/halium/halium-boot.img
    command   : heimdall flash --BOOT $ARTIFACT_DIR/generated/halium/halium-boot.img
    rootfs    : $ROOTFS_IMG must also be placed on the userdata partition,
                conventionally as /data/rootfs.img
    recovery  : heimdall flash --BOOT $STOCK_BOOT

==> Bring-up on the host, once the phone is in Download Mode and flashed
    tools/flash-heimdall.sh                  # dry-run: prints the exact command
    I_HAVE_A_RECOVERY_PATH=YES tools/flash-heimdall.sh --execute
    dmesg -w &                               # TEST 1: does USB enumerate at all?
    lsusb
    tools/usb-debug.sh --setup               # TEST 2/3: detect the link, set $HOST_IP
    ping $DEVICE_IP                          # TEST 3
    tools/ssh-device.sh                      # TEST 4/5

The build script never flashes, repartitions, erases userdata, or downloads a PIT.
EOF
}

case "$MODE" in
    kernel) build_kernel ;;
    halium-kernel)
        "$ROOT/tools/build-halium-kernel.sh"
        ;;
    boot) build_boot ;;
    rootfs) prepare_rootfs ;;
    halium-install) halium_install ;;
    all)
        build_kernel
        # Build the headless profile and install it as the d2q prebuilt so the
        # boot image below actually contains it.
        "$ROOT/tools/build-halium-kernel.sh"
        install_halium_kernel
        build_boot
        prepare_rootfs
        ;;
esac

if ((FLASH_REQUESTED)); then
    printf '\n--flash was requested, but flashing is intentionally disabled in this helper.\n'
    printf 'After reviewing the image and recovery path, run the Heimdall command manually.\n'
    exit 2
fi

print_next_steps
