#!/bin/bash
# Wait for a manual reboot, then instantly snapshot the Samsung logfs FAT12 volume.
# Read-only against the device: `dd if=` only.
set -u
SER=RF8M82H3NFM
OUT=/home/usb/artifacts/device/logs/iter3
mkdir -p "$OUT"

say() { echo "[$(date -u +%H:%M:%SZ)] $*"; }

say "stage 1: waiting for $SER to go OFFLINE (boot in progress)..."
gone=0
for i in $(seq 1 240); do          # up to 60 min
  sleep 15
  st=$(adb -s $SER get-state 2>/dev/null | tr -d '\r')
  if [ -z "$st" ]; then
    say "  device OFFLINE at +$((i*15))s  <- ABL has run, logfs written"
    gone=1
    break
  fi
  if [ $((i % 4)) -eq 0 ]; then say "  still online (+$((i*15))s)"; fi
done

if [ "$gone" != "1" ]; then say "ABORT: device never went offline"; exit 1; fi

say "stage 2: waiting for $SER to come back ONLINE (recovery/TWRP)..."
back=0
for i in $(seq 1 160); do          # up to 40 min
  sleep 15
  st=$(adb -s $SER get-state 2>/dev/null | tr -d '\r')
  if [ -n "$st" ]; then
    say "  device back ONLINE at +$((i*15))s (state=$st)"
    back=1
    break
  fi
  if [ $((i % 8)) -eq 0 ]; then say "  still offline (bootlooping, +$((i*15))s)"; fi
done

if [ "$back" != "1" ]; then say "ABORT: device never came back online"; exit 1; fi

say "stage 3: snapshotting logfs"
# let ADB settle a little so the block device is not busy
sleep 10
adb -s $SER shell 'dd if=/dev/block/bootdevice/by-name/logfs bs=65536 count=128 2>/dev/null' \
    > "$OUT/logfs-POST.img" 2>/dev/null
sz=$(stat -c%s "$OUT/logfs-POST.img")
say "  logfs-POST.img = $sz bytes"
if [ "$sz" != "8388608" ]; then say "  WARNING: unexpected size"; fi
sha256sum "$OUT/logfs-POST.img" | tee "$OUT/logfs-POST.sha256"
sha256sum "$OUT/logfs-PRE.img"  | tee "$OUT/logfs-PRE.sha256"

say "stage 4: extracting UefiLog1.txt"
mcopy -n -i "$OUT/logfs-POST.img" ::UefiLog1.txt "$OUT/UefiLog1-POST.txt" 2>&1
mcopy -n -i "$OUT/logfs-PRE.img"  ::UefiLog1.txt "$OUT/UefiLog1-PRE.txt"  2>&1
ls -l "$OUT"/UefiLog1-*.txt
sha256sum "$OUT"/UefiLog1-*.txt

say "stage 5: byte-level diff of the FAT volume (which regions changed)"
cmp -l "$OUT/logfs-PRE.img" "$OUT/logfs-POST.img" 2>/dev/null | wc -l | sed 's/^/  differing bytes: /'

say "stage 6: the bootmode/cmdline the device came back with"
adb -s $SER shell 'getprop ro.bootmode; getprop sys.boot_completed; cat /proc/cmdline' 2>/dev/null | tr -d '\r'

say "DONE"
