#!/bin/bash
# Build the GPU experiment kernel.
#
# WHY TMPDIR IS ON THE SECOND DISK (2026-10-18):
#   The build runs inside the `kernel-build` distrobox container.  That
#   container's root filesystem is a podman OVERLAY whose backing store is
#   /home (i.e. /dev/sdb2[@home]).  When /home fills up, every write into the
#   overlay fails with ENOSPC -- including the compiler's temporary files --
#   even though the kernel output directory (KERNEL_OUT) is on a different,
#   non-full filesystem.
#
#   Symptom seen: "fatal error: error in backend: IO failure on output stream:
#   No space left on device" and "tee: .../build-gpu.txt: No space left on
#   device", with `df` inside the container showing the overlay at 100%.
#
#   Fix: keep the bulk of the write traffic (compiler temporaries, and the
#   kernel output itself) on the second disk, and leave the system drive with
#   headroom.  The distrobox passes the host environment through, so exporting
#   TMPDIR here is enough.
#
# NOTE: this script must be run FROM THE HOST.  It enters the container
# itself.  The container lacks python2; there is a shim at
# /home/usb/.opencode-tmp/bin/python2 -> python2.7.
cd /home/usb
export KERNEL_DIR=/home/usb/ShinyKernel-gpu-exp/Kernel
export BASELINE_CONFIG=/home/usb/ShinyKernel/Kernel/out-bt/.config
export FRAGMENT=/home/usb/configs/d2q_debug.config
export KERNEL_OUT=/home/usb/mnt-sda/a6xx-build/out-gpu
export KERNEL_LOG=/home/usb/mnt-sda/a6xx-build/build-gpu.txt
export BUILD_JOBS=8
export HALIUM_CHECK=/nonexistent-skip

# Second disk for all scratch/temporary output.
export TMPDIR=/home/usb/mnt-sda/tmpbuild
mkdir -p "$TMPDIR"

./tools/build-halium-kernel.sh
