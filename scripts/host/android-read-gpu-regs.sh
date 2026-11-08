#!/bin/sh
# Read the GPU's register blocks from the WORKING Android device via /dev/mem
# (the diagnostic kernel has CONFIG_DEVMEM=y with STRICT/IO_STRICT_DEVMEM off).
#
# Register blocks, from the d2q device tree (sm8150-gpu.dtsi):
#   kgsl_3d0 reg_memory  0x02C00000  0x40000   (the a6xx register window)
#   cx_dbgc              0x02C61000  0x00800   (CX debug block)
#   qdss_gfx             0x06900000  0x44000   (QDSS / GPU bus)
#
# The a6xx register offsets are decoded on the host with the tree's
# a6xx.xml.h, so the same decoder works for the Debian side.
set -e
OUT=/data/local/tmp
mkdir -p $OUT

mknod /dev/mem c 1 1 2>/dev/null || true
ls -l /dev/mem

read_block() {
	name=$1; addr=$2; size=$3; pages=$((size / 4096))
	echo "=== $name: 0x$addr .. 0x$((addr + size)) ($size bytes) ==="
	dd if=/dev/mem of=$OUT/$name.bin bs=4096 skip=$((addr / 4096)) count=$pages 2>&1 | tail -1
	ls -l $OUT/$name.bin
}

read_block gpu_regs 0x02C00000 0x40000
read_block cx_dbgc  0x02C61000 0x00800
read_block qdss_gfx 0x06900000 0x44000

echo "=== also: the KGSL GPU model and the clock state (for the record) ==="
cat /sys/class/kgsl/kgsl-3d0/gpu_model 2>/dev/null || true
cat /sys/class/kgsl/kgsl-3d0/gpuclk 2>/dev/null || true
cat /sys/class/kgsl/kgsl-3d0/gpubusy 2>/dev/null || true
cat /sys/kernel/debug/kgsl/kgsl-3d0/active_cnt 2>/dev/null || true

echo "=== DONE ==="
