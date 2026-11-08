#!/usr/bin/env bash
# Control rebuild with the PRISTINE source, to attribute the iteration-5 kernel
# delta. Builds into a separate output tree and a scratch artifact dir so the
# shipped iteration-5 artifacts are never touched. The fixed source is always
# restored on exit.
set -Eeuo pipefail
cd /home/usb

K=/home/usb/ShinyKernel/Kernel
CONTROL_OUT=/home/usb/artifacts/control-rebuild/out-control
CONTROL_ART=/home/usb/artifacts/control-rebuild/artifacts
FIXED=/home/usb/artifacts/iter5-fixed-source

# The pristine snapshots are mode 444; `cp -a` would propagate that and make the
# next write fail. Force write permission on both sides of every copy.
restore() {
  chmod u+w "$K/init/initramfs.c" "$K/arch/arm64/kernel/setup.c" 2>/dev/null || true
  cp "$FIXED/initramfs.c" "$K/init/initramfs.c"
  cp "$FIXED/setup.c"     "$K/arch/arm64/kernel/setup.c"
  echo "### fixed source restored"
  sha256sum "$K/init/initramfs.c" "$K/arch/arm64/kernel/setup.c"
  echo "  expected 784ae7a3e5cce5ebae10b149da994c502d71bd23ed1548fc67703b5942271fc7  initramfs.c"
  echo "  expected a23d3a176107e970a3c6329fb674540bac7483c84f5edd2c9b54680b2f383b69  setup.c"
}
trap restore EXIT

echo "### 1. swap in the pristine source"
chmod u+w "$K/init/initramfs.c" "$K/arch/arm64/kernel/setup.c"
cp artifacts/device/iter5-baseline/initramfs.c.orig "$K/init/initramfs.c"
cp artifacts/device/iter5-baseline/setup.c.orig     "$K/arch/arm64/kernel/setup.c"
sha256sum "$K/init/initramfs.c" "$K/arch/arm64/kernel/setup.c"
echo "  pristine initramfs.c expected 1a9af19668584dc664c28c640e02b6a03cc37a2fa5ecd1900d818bb67c775806"
echo "  pristine setup.c     expected f7d6612e8263f02990fc2ae0395cb5b66ebd4d9af50d8134d1312ac21f5189e6"

echo
echo "### 2. build the control kernel (pristine source, same defconfig, -j2)"
rm -rf "$CONTROL_OUT"
mkdir -p "$CONTROL_ART"
KERNEL_OUT="$CONTROL_OUT" ARTIFACT_DIR="$CONTROL_ART" \
    bash tools/build-halium.sh --kernel-only

echo
echo "### 3. compare the control build against the preserved known-good baseline"
echo "control  Image-dtb: $(sha256sum $CONTROL_OUT/arch/arm64/boot/Image-dtb | cut -d' ' -f1)"
echo "baseline Image-dtb: $(sha256sum artifacts/known-good/shinykernel-baseline/Image-dtb | cut -d' ' -f1)"
if cmp -s "$CONTROL_OUT/arch/arm64/boot/Image-dtb" artifacts/known-good/shinykernel-baseline/Image-dtb; then
  echo "RESULT: REPRODUCIBLE - the control build is byte-identical to the baseline."
  echo "        => the entire iter2->iter5 kernel delta is attributable to the two source changes."
else
  echo "RESULT: NOT byte-identical to the baseline."
  python3 - <<PY
a=open("$CONTROL_OUT/arch/arm64/boot/Image-dtb","rb").read()
b=open("/home/usb/artifacts/known-good/shinykernel-baseline/Image-dtb","rb").read()
n=min(len(a),len(b)); runs=0; tot=0; i=0
while i<n:
    if a[i]!=b[i]:
        j=i
        while j<n and a[j]!=b[j]: j+=1
        runs+=1; tot+=j-i; i=j
    else: i+=1
print("  differing bytes: %d in %d runs (sizes %d vs %d)"%(tot,runs,len(a),len(b)))
PY
fi
echo
echo "### 4. control config vs baseline config"
sha256sum "$CONTROL_OUT/.config" artifacts/known-good/shinykernel-baseline/config
