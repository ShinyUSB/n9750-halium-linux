#!/bin/bash
# clang wrapper for the Snapdragon LLVM: inject the aarch64 target and strip
# GCC-only flags that the Qualcomm clang rejects outright.
ARGS=()
for a in "$@"; do
  case "$a" in
    -fno-var-tracking-assignments|-fno-var-tracking) ;;
    *) ARGS+=("$a") ;;
  esac
done
exec /home/usb/mnt-sda/android-kernel/Kernel/toolchains/Snapdragon-LLVM/bin/clang --target=aarch64-linux-gnu "${ARGS[@]}"
