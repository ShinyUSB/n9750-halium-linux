#!/usr/bin/env bash
# Verify a prepared Halium rootfs without booting it.
#
# No device is contacted and nothing is flashed. Where a static qemu-aarch64
# emulator is available the rootfs binaries are actually *executed*, so this
# catches wrong architecture, missing shared libraries and a broken sshd_config
# long before the phone is involved. Where it is not available, the checks
# degrade to static ones and say so.
set -Eeuo pipefail
export LC_ALL=C

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

ROOTFS_DIR=${ROOTFS_DIR:-"$ROOT/artifacts/generated/halium-rootfs-arm64"}
QEMU=${QEMU:-"$ROOT/artifacts/tools/qemu-aarch64-static"}
INITRAMFS=${INITRAMFS:-"$ROOT/artifacts/initramfs/initrd.img-touch-arm64"}
ARCH=${ARCH:-aarch64}

pass=0
fail_count=0
skip=0

ok()   { printf '  ok    %s\n' "$*"; pass=$((pass + 1)); }
bad()  { printf '  FAIL  %s\n' "$*"; fail_count=$((fail_count + 1)); }
skipt(){ printf '  skip  %s\n' "$*"; skip=$((skip + 1)); }

[[ -d "$ROOTFS_DIR" ]] || { printf 'error: rootfs not found: %s\n' "$ROOTFS_DIR" >&2; exit 1; }

printf '==> Rootfs: %s\n' "$ROOTFS_DIR"
printf '==> Size:    %s\n' "$(du -sh "$ROOTFS_DIR" | cut -f1)"

# ---------------------------------------------------------------------------
# 1. Architecture
# ---------------------------------------------------------------------------
printf '\n--- architecture ---\n'
# `file` describes an aarch64 binary as "ELF 64-bit LSB pie executable, ARM
# aarch64", so match on the full token rather than the leading "ARM".
elf_desc=$(file -b "$ROOTFS_DIR/bin/bash" 2>/dev/null)
case "$ARCH" in
    aarch64) [[ "$elf_desc" == *aarch64* ]] ;;
    arm)     [[ "$elf_desc" == *ARM* && "$elf_desc" != *aarch64* ]] ;;
    *)       [[ "$elf_desc" == *"$ARCH"* ]] ;;
esac && ok "/bin/bash is $ARCH" || bad "/bin/bash is not $ARCH: $elf_desc"

# Every ELF in the tree must match the target architecture. Anything else means
# a foreign package leaked in.
elf_matches() {
    case "$1" in
        *ELF*) ;;
        *) return 0 ;;   # not an ELF file; nothing to check
    esac
    case "$ARCH" in
        aarch64) [[ "$1" == *aarch64* ]] ;;
        arm)     [[ "$1" == *ARM* && "$1" != *aarch64* ]] ;;
        *)       [[ "$1" == *"$ARCH"* ]] ;;
    esac
}
foreign=0
sample=""
while IFS= read -r -d '' f; do
    desc=$(file -b "$f" 2>/dev/null)
    if ! elf_matches "$desc"; then
        foreign=$((foreign + 1))
        [[ -z "$sample" ]] && sample="$f: $desc"
    fi
done < <(find "$ROOTFS_DIR/usr/bin" "$ROOTFS_DIR/usr/sbin" "$ROOTFS_DIR/usr/lib/systemd" \
            -type f -print0 2>/dev/null)
if [[ "$foreign" -eq 0 ]]; then
    ok "all ELF binaries in bin/sbin/systemd are $ARCH"
else
    bad "$foreign ELF binaries are not $ARCH (first: $sample)"
fi

# ---------------------------------------------------------------------------
# 2. Merged-/usr consistency
# ---------------------------------------------------------------------------
printf '\n--- merged /usr layout ---\n'
for d in bin sbin lib; do
    if [[ -L "$ROOTFS_DIR/$d" ]]; then
        t=$(readlink "$ROOTFS_DIR/$d")
        if [[ "$t" == "usr/$d" ]]; then
            ok "/$d -> $t"
        else
            bad "/$d -> $t (expected usr/$d)"
        fi
    else
        bad "/$d is not a symlink into /usr; a non-merged package was unpacked"
    fi
done

# ---------------------------------------------------------------------------
# 3. Required userspace (the task's minimum list)
# ---------------------------------------------------------------------------
printf '\n--- required userspace ---\n'
for f in bin/bash bin/cat bin/mount usr/bin/ip sbin/modprobe sbin/lsmod \
         sbin/init lib/systemd/systemd usr/lib/systemd/systemd-udevd \
         usr/bin/udevadm usr/sbin/sshd usr/bin/ps; do
    if [[ -x "$ROOTFS_DIR/$f" ]]; then
        ok "/$f"
    else
        bad "/$f missing or not executable"
    fi
done

# ---------------------------------------------------------------------------
# 4. No graphical or Android stack
# ---------------------------------------------------------------------------
printf '\n--- headless / no Android ---\n'
for forbidden in usr/bin/Xorg usr/bin/wayland usr/lib/xorg \
                 usr/lib/lxc usr/bin/lxc-start var/lib/lxc/android/system.img \
                 usr/lib/android usr/bin/surfaceflinger; do
    if [[ -e "$ROOTFS_DIR/$forbidden" ]]; then
        bad "forbidden component present: /$forbidden"
    else
        ok "absent: /$forbidden"
    fi
done
if [[ -d "$ROOTFS_DIR/usr/share/wayland-sessions" ]]; then
    bad "desktop session definitions present"
else
    ok "no desktop session definitions"
fi

# ---------------------------------------------------------------------------
# 5. Halium initramfs contract
# ---------------------------------------------------------------------------
printf '\n--- Halium initramfs contract ---\n'
# The initramfs execs `run-init ${rootmnt} /sbin/init`.
if [[ -e "$ROOTFS_DIR/sbin/init" ]]; then
    ok "/sbin/init present ($(readlink -f "$ROOTFS_DIR/sbin/init" | sed "s|$ROOTFS_DIR||"))"
else
    bad "/sbin/init missing; the initramfs would fall back to /bin/sh"
fi

# The initramfs does `mount --move /android-rootfs ${rootmnt}/var/lib/lxc/android/rootfs`.
# mount(8) will not create that destination, so the parent must pre-exist.
if [[ -d "$ROOTFS_DIR/var/lib/lxc/android" ]]; then
    ok "/var/lib/lxc/android exists (mount --move destination)"
else
    bad "/var/lib/lxc/android missing; 'mount --move' would fail in the initramfs"
fi

# Without this marker the initramfs remounts the rootfs read-only.
if [[ -f "$ROOTFS_DIR/.writable_image" ]]; then
    ok ".writable_image present (rootfs stays read-write)"
else
    bad ".writable_image missing; the rootfs would boot read-only"
fi

# The initramfs moves its own /proc, /sys, /dev and /run into the new root.
for d in proc sys dev run; do
    if [[ -d "$ROOTFS_DIR/$d" ]]; then
        ok "/$d exists"
    else
        bad "/$d missing; the initramfs mount --move would fail"
    fi
done

# Do not let the initramfs replace /etc/fstab with a generated bind-mount fstab.
if [[ -e "$ROOTFS_DIR/etc/system-image/writable-paths" ]]; then
    bad "etc/system-image/writable-paths exists; the initramfs would bind-mount over /etc/fstab"
else
    ok "no writable-paths (initramfs leaves /etc/fstab alone)"
fi

# ---------------------------------------------------------------------------
# 6. RNDIS and SSH configuration
# ---------------------------------------------------------------------------
printf '\n--- RNDIS and SSH ---\n'
device_ip=$(sed -n 's/^Address=\([0-9.]*\)\/.*/\1/p' \
    "$ROOTFS_DIR/etc/systemd/network/10-d2q-rndis.network" 2>/dev/null | head -1)
if [[ "$device_ip" == "10.15.19.82" ]]; then
    ok "static device address is 10.15.19.82"
else
    bad "static device address is '${device_ip:-unset}', expected 10.15.19.82"
fi
for f in usr/bin/d2q-rndis lib/systemd/system/d2q-rndis.service \
         etc/systemd/network/10-d2q-rndis.network \
         etc/ssh/sshd_config root/.ssh/authorized_keys; do
    if [[ -s "$ROOTFS_DIR/$f" ]]; then
        ok "/$f"
    else
        bad "/$f missing or empty"
    fi
done
enabled=$(readlink "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants/d2q-rndis.service" 2>/dev/null || true)
if [[ -n "$enabled" && "$enabled" == /* && -e "$ROOTFS_DIR$enabled" ]]; then
    ok "d2q-rndis.service enabled -> $enabled (target exists in the rootfs)"
else
    bad "d2q-rndis.service is not enabled correctly (link='${enabled:-none}')"
fi
enabled=$(readlink "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants/systemd-networkd.service" 2>/dev/null || true)
if [[ -n "$enabled" && "$enabled" == /* && -e "$ROOTFS_DIR$enabled" ]]; then
    ok "systemd-networkd.service enabled -> $enabled"
else
    bad "systemd-networkd.service is not enabled correctly (link='${enabled:-none}')"
fi
enabled=$(readlink "$ROOTFS_DIR/etc/default/target" 2>/dev/null || true)
if [[ "$enabled" == "/lib/systemd/system/multi-user.target" ]]; then
    ok "default boot target is multi-user.target"
else
    bad "default boot target is '${enabled:-unset}'"
fi

# Root login is key-only, so the key must actually be there.
if grep -q "^PermitRootLogin prohibit-password" "$ROOTFS_DIR/etc/ssh/sshd_config"; then
    ok "PermitRootLogin is key-only"
else
    bad "PermitRootLogin is not key-only"
fi
if grep -qE "^PubkeyAcceptedAlgorithms.*rsa-sha2-512" "$ROOTFS_DIR/etc/ssh/sshd_config"; then
    ok "rsa-sha2-512 is accepted (works with OpenSSH 10.x clients)"
else
    bad "rsa-sha2-512 is not accepted; modern clients would refuse to log in"
fi
if grep -q "^ssh-rsa " "$ROOTFS_DIR/root/.ssh/authorized_keys"; then
    ok "root authorized_keys present (ssh-rsa, upgraded to rsa-sha2 at login)"
else
    bad "root authorized_keys is missing or not ssh-rsa"
fi

# ---------------------------------------------------------------------------
# 7. Execute the binaries (needs the static emulator)
# ---------------------------------------------------------------------------
printf '\n--- executing ARM64 binaries under qemu ---\n'
if [[ ! -x "$QEMU" ]]; then
    skipt "no static emulator at $QEMU; static checks only"
    skipt "(extract it from the Debian amd64 qemu-user-static package to enable)"
else
    run_arm64() {   # label path args...
        local label=$1 path=$2; shift 2
        local out rc
        set +e
        out=$("$QEMU" -L "$ROOTFS_DIR" "$ROOTFS_DIR/$path" "$@" 2>&1)
        rc=$?
        set -e
        printf '%s' "$out" > /tmp/.d2q-verify-out
        LAST_OUT=$out
        return $rc
    }

    # systemd
    if run_arm64 systemd usr/lib/systemd/systemd --version; then
        ok "systemd runs: $(printf '%s' "$LAST_OUT" | head -1)"
    else
        bad "systemd --version failed: $LAST_OUT"
    fi

    # sshd, including real config validation
    if run_arm64 sshd usr/sbin/sshd -V; then
        ok "sshd runs: $LAST_OUT"
    else
        bad "sshd -V failed: $LAST_OUT"
    fi
    if run_arm64 "sshd -t" usr/sbin/sshd -t -f "$ROOTFS_DIR/etc/ssh/sshd_config"; then
        ok "sshd accepts /etc/ssh/sshd_config"
    else
        bad "sshd rejects /etc/ssh/sshd_config: $LAST_OUT"
    fi

    # udev
    if run_arm64 udevadm usr/bin/udevadm --version; then
        ok "udevadm runs (udev $LAST_OUT)"
    else
        bad "udevadm failed: $LAST_OUT"
    fi

    # iproute2, kmod, coreutils, bash, procps
    if run_arm64 ip usr/bin/ip -V; then
        ok "iproute2 runs: $LAST_OUT"
    else
        bad "ip failed: $LAST_OUT"
    fi
    if run_arm64 kmod usr/bin/kmod --version; then
        ok "kmod runs: $(printf '%s' "$LAST_OUT" | head -1)"
    else
        bad "kmod failed: $LAST_OUT"
    fi
    if run_arm64 bash usr/bin/bash -c 'echo ok'; then
        ok "bash runs: $LAST_OUT"
    else
        bad "bash failed: $LAST_OUT"
    fi
    if run_arm64 ps usr/bin/ps --version; then
        ok "procps runs: $(printf '%s' "$LAST_OUT" | head -1)"
    else
        bad "ps failed: $LAST_OUT"
    fi
    if run_arm64 mount usr/bin/mount --version; then
        ok "mount runs: $(printf '%s' "$LAST_OUT" | head -1)"
    else
        bad "mount failed: $LAST_OUT"
    fi
    # e2fsprogs: the Halium initramfs relies on these semantics, and the
    # rootfs uses the same tools to service the userdata filesystem.
    if run_arm64 e2fsck usr/sbin/e2fsck -V; then
        ok "e2fsck runs: $(printf '%s' "$LAST_OUT" | head -1)"
    else
        bad "e2fsck failed: $LAST_OUT"
    fi
    rm -f /tmp/.d2q-verify-out
fi

# ---------------------------------------------------------------------------
# 8. The initramfs this rootfs has to be booted by
# ---------------------------------------------------------------------------
printf '\n--- initramfs ---\n'
if [[ -f "$INITRAMFS" ]]; then
    ok "initramfs present: $(basename "$INITRAMFS") ($(stat -c %s "$INITRAMFS") bytes)"
    if [[ -x "$QEMU" ]]; then
        # The initramfs loop-mounts the rootfs image, so the kernel must have
        # loop support. Confirm that from the configuration we actually built.
        cfg="$ROOT/n9750-halium/device/samsung/d2q/prebuilt/halium-kernel.config"
        [[ -f "$cfg" ]] || cfg="$ROOT/ShinyKernel/Kernel/out-halium/.config"
        if [[ -f "$cfg" ]]; then
            for sym in CONFIG_BLK_DEV_LOOP CONFIG_BLK_DEV_INITRD CONFIG_RD_GZIP \
                       CONFIG_DEVTMPFS CONFIG_DEVTMPFS_MOUNT CONFIG_EXT4_FS; do
                if grep -qE "^${sym}=y$" "$cfg"; then
                    ok "kernel $sym=y (required to loop-mount rootfs.img)"
                else
                    bad "kernel $sym is not set; the initramfs cannot mount rootfs.img"
                fi
            done
        else
            skipt "no kernel config found to check loop/ext4 support"
        fi
    fi
else
    bad "initramfs not found: $INITRAMFS"
fi

printf '\n==> %d passed, %d failed, %d skipped\n' "$pass" "$fail_count" "$skip"
if [[ "$fail_count" -gt 0 ]]; then
    printf '==> Rootfs verification FAILED. This is a BUILD-TIME result, not a device test.\n'
    exit 1
fi
printf '==> Rootfs verification passed. This is still a BUILD-TIME result:\n'
printf '    nothing here proves the phone boots, enumerates USB, or accepts SSH.\n'
