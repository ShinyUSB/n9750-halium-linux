#!/usr/bin/env bash
# Build a separate, non-destructive Halium-profile kernel output tree.
set -Eeuo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
KERNEL_DIR=${KERNEL_DIR:-"$ROOT/ShinyKernel/Kernel"}
BASELINE_CONFIG=${BASELINE_CONFIG:-"$KERNEL_DIR/out/.config"}
FRAGMENT=${FRAGMENT:-"$ROOT/configs/d2q_halium_headless.config"}
KERNEL_OUT=${KERNEL_OUT:-"$KERNEL_DIR/out-halium"}
KERNEL_LOG=${KERNEL_LOG:-"$KERNEL_DIR/build_log_halium-headless.txt"}
BUILD_JOBS=${BUILD_JOBS:-2}
HALIUM_CHECK=${HALIUM_CHECK:-"$ROOT/n9750-halium/bootable/halium-boot/check-kernel-config"}

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

[[ -f "$KERNEL_DIR/build.txt" ]] || fail "missing authoritative build.txt"
[[ -f "$BASELINE_CONFIG" ]] || fail "missing baseline config: $BASELINE_CONFIG"
[[ -f "$FRAGMENT" ]] || fail "missing Halium config fragment: $FRAGMENT"
[[ -x "$KERNEL_DIR/toolchains/Snapdragon-LLVM/bin/clang" ]] || fail "missing Snapdragon clang"
[[ -x "$KERNEL_DIR/toolchains/gcc-aarch64/bin/aarch64-linux-android-gcc" ]] || fail "missing GCC wrapper"
command -v distrobox >/dev/null 2>&1 || fail "distrobox is not installed"
distrobox list | awk -F'|' '$2 ~ /kernel-build/ { found=1 } END { exit(found ? 0 : 1) }' \
    || fail "the kernel-build distrobox does not exist"

mkdir -p "$KERNEL_OUT"
# The baseline output is the source of truth. Never merge into its .config.
cp -a "$BASELINE_CONFIG" "$KERNEL_OUT/.config"

printf '==> Merging Halium headless fragment into %s\n' "$KERNEL_OUT/.config"
printf '    fragment: %s\n' "$FRAGMENT"
printf '    jobs:     %s\n' "$BUILD_JOBS"

distrobox enter kernel-build --no-tty -- bash -s -- \
    "$KERNEL_DIR" "$KERNEL_OUT" "$FRAGMENT" "$KERNEL_LOG" "$BUILD_JOBS" <<'CONTAINER'
set -Eeuo pipefail
kernel_dir=$1
kernel_out=$2
fragment=$3
kernel_log=$4
build_jobs=$5

cd "$kernel_dir"
export ARCH=arm64
export SUBARCH=arm64
export CROSS_COMPILE="$kernel_dir/toolchains/gcc-aarch64/bin/aarch64-linux-android-"
export REAL_CC="$kernel_dir/toolchains/Snapdragon-LLVM/bin/clang"
export KCONFIG_CONFIG="$kernel_out/.config"

KCONFIG_CONFIG="$kernel_out/.config" \
    scripts/kconfig/merge_config.sh -m "$kernel_out/.config" "$fragment"

make -C "$kernel_dir" O="$kernel_out" \
    KCONFIG_CONFIG="$kernel_out/.config" \
    CONFIG_BUILD_ARM64_DT_OVERLAY=y \
    CLANG_TRIPLE=aarch64-linux-gnu- \
    CROSS_COMPILE="$CROSS_COMPILE" \
    olddefconfig

make -C "$kernel_dir" O="$kernel_out" \
    KCONFIG_CONFIG="$kernel_out/.config" \
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

[[ -f "$KERNEL_OUT/arch/arm64/boot/Image-dtb" ]] || fail "Halium-profile Image-dtb was not produced"
[[ -f "$KERNEL_OUT/.config" ]] || fail "Halium-profile config was not produced"

printf '==> Halium-profile kernel artifacts:\n'
sha256sum "$KERNEL_OUT/arch/arm64/boot/Image" "$KERNEL_OUT/arch/arm64/boot/Image-dtb" "$KERNEL_OUT/.config"

if [[ -x "$HALIUM_CHECK" ]]; then
    report="$KERNEL_OUT/halium-config-check.txt"
    "$HALIUM_CHECK" "$KERNEL_OUT/.config" >"$report" 2>&1
    printf '\n==> Upstream check summary (non-fatal for the headless subset):\n'
    grep -E 'Config file checked|errors that|Halium check' "$report" || true
    printf 'Full report: %s\n' "$report"
fi
