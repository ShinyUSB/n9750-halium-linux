#!/bin/bash
# Build the Android (ShinyKernel #1) kernel with CONFIG_DEVMEM enabled, so
# the GPU's registers can be read directly on the WORKING device.
#
# WHY /home must not be used for temporaries:
#   /home is 99% full (1.3 GB).  The distrobox container's root is a podman
#   overlay backed by /home, so compiler temporaries there fail with ENOSPC
#   even though the kernel output directory is on mnt-sda.  Hence TMPDIR.
#
# The build follows /home/usb/mnt-sda/android-kernel/Kernel/build.txt.
set -e
KDIR=/home/usb/mnt-sda/android-kernel/Kernel
OUT=$KDIR/out
export TMPDIR=/home/usb/mnt-sda/android-build-tmp
mkdir -p "$TMPDIR"

export ARCH=arm64
export SUBARCH=arm64
export CROSS_COMPILE=$KDIR/toolchains/gcc-aarch64/bin/aarch64-linux-android-
export REAL_CC=$KDIR/toolchains/Snapdragon-LLVM/bin/clang
export CLANG_FLAGS="-mcpu=kryo -mllvm -vectorize-loops -mllvm -vectorize-slp -mllvm -enable-qcom-loop-vectorizer -mllvm -qcom-unroll-loops -mllvm -polly-ast -mllvm -polly-vectorizer=stripmine"

echo "=== defconfig (with CONFIG_DEVMEM=y) ==="
make -C "$KDIR" O="$OUT" DTC_EXT="$KDIR/tools/dtc" \
     CONFIG_BUILD_ARM64_DT_OVERLAY=y CLANG_TRIPLE=aarch64-linux-gnu- \
     CROSS_COMPILE="$CROSS_COMPILE" d2q_chn_hk_defconfig

echo "=== the config that will be built ==="
grep -E "^CONFIG_DEVMEM|^# CONFIG_DEVMEM|STRICT_DEVMEM" "$OUT/.config"

echo "=== build ==="
make -C "$KDIR" O="$OUT" \
    ARCH=arm64 \
    DTC_EXT="$KDIR/tools/dtc" \
    CONFIG_BUILD_ARM64_DT_OVERLAY=y \
    CLANG_TRIPLE=aarch64-linux-gnu- \
    CC="$KDIR/toolchains/Snapdragon-LLVM/bin/clang" \
    REAL_CC="$KDIR/toolchains/Snapdragon-LLVM/bin/clang" \
    CROSS_COMPILE="$CROSS_COMPILE" \
    KCFLAGS="--target=aarch64-linux-gnu -march=armv8.2-a+crypto+fp+simd -pipe -O3" \
    -j8 2>&1 | tail -40

echo "=== artifacts ==="
ls -l "$OUT/arch/arm64/boot/" | head
sha256sum "$OUT/arch/arm64/boot/Image-dtb" 2>/dev/null || true
