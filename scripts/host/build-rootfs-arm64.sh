#!/usr/bin/env bash
# Build a minimal, headless ARM64 Debian rootfs that the Halium initramfs can
# boot into, without any Android system image.
#
# Why this exists
# ---------------
# The published Halium reference rootfs is 32-bit armhf. The d2q kernel is
# ARM64. CONFIG_COMPAT makes the armhf tree run, but the supported Halium
# pairing is arm64 rootfs + arm64 system image, and the armhf build also
# targets a 2017 Ubuntu base against a halium-10 tree.
#
# How it is built
# --------------
#   1. Base: the official debian:bookworm-slim ARM64 root filesystem, exported
#      with podman and cached under artifacts/base/ with a recorded SHA256.
#   2. Packages: a dependency closure is resolved from the Debian bookworm
#      Packages indexes by tools/debdeps.py, downloaded, and verified against
#      the index SHA256 before anything is unpacked.
#   3. Unpacking is archive extraction (ar + tar) only. No qemu, no dpkg, no
#      root, and no maintainer scripts are run, because this host cannot
#      execute ARM64 binaries.
#   4. Halium boot wiring: /sbin/init, the /var/lib/lxc and /run mountpoints
#      the initramfs expects, a writable-rootfs marker, sshd, and RNDIS.
#
# Nothing is flashed and no device is contacted. See PORTING-STATUS.md for the
# trace showing why no system.img is required.
set -Eeuo pipefail
export LC_ALL=C

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

ARTIFACTS=${ARTIFACTS:-"$ROOT/artifacts"}
BASE_DIR=${BASE_DIR:-"$ARTIFACTS/base"}
DEB_CACHE=${DEB_CACHE:-"$ARTIFACTS/debs"}
ROOTFS_DIR=${ROOTFS_DIR:-"$ARTIFACTS/generated/halium-rootfs-arm64"}
KEY_DIR=${KEY_DIR:-"$ARTIFACTS/keys"}
DEB_MIRROR=${DEB_MIRROR:-"http://deb.debian.org/debian"}
DEB_DIST=${DEB_DIST:-bookworm}
DEB_ARCH=${DEB_ARCH:-arm64}
DEVICE_IP=${DEVICE_IP:-10.15.19.82}
DEVICE_PREFIX=${DEVICE_PREFIX:-24}
HOST_IP=${HOST_IP:-10.15.19.1}
HOST_PREFIX=${HOST_PREFIX:-24}

# Pinned base image. Override BASE_TAR to supply a different ARM64 Debian rootfs.
BASE_IMAGE=${BASE_IMAGE:-docker.io/library/debian:bookworm-slim}
BASE_TAR=${BASE_TAR:-"$BASE_DIR/debian-bookworm-slim-arm64.tar"}
BASE_SHA256=${BASE_SHA256:-e8af8f971be2eb637bd178aaeb891a941dafd9918c385682ed064e87d7dbfa6e}

# The Halium headless target. Deliberately excludes any graphical, audio or
# Android component, and excludes Recommends by construction (debdeps.py
# follows Depends/Pre-Depends only).
PACKAGES=(
    systemd            # init, journald
    systemd-sysv       # /sbin/init
    udev               # systemd-udevd, the /dev and hotplug daemon
    iproute2           # ip, needed by RNDIS setup and systemd-networkd
    openssh-server     # rsa-sha2 capable SSH
    openssh-sftp-server
    kmod               # modprobe/lsmod
    procps             # ps, top, free
)

usage() {
    cat <<'EOF'
Usage: tools/build-rootfs-arm64.sh [--rootfs DIR] [--keep]

Builds a minimal headless ARM64 Debian rootfs bootable by the Halium
initramfs. Idempotent: re-running does not modify an already-prepared tree.

  --rootfs DIR   output rootfs directory
  --keep         keep the existing tree instead of re-extracting the base
  -h, --help     this text

This script NEVER flashes and NEVER contacts a device.
EOF
}

KEEP=0
while (($#)); do
    case "$1" in
        --rootfs) ROOTFS_DIR=$2; shift 2 ;;
        --keep) KEEP=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'error: unknown argument: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done

fail() { printf 'error: %s\n' "$*" >&2; exit 1; }
note() { printf '==> %s\n' "$*"; }

for cmd in curl tar ar python3 ssh-keygen; do
    command -v "$cmd" >/dev/null 2>&1 || fail "required host command is missing: $cmd"
done
[[ -f "$SCRIPT_DIR/debdeps.py" ]] || fail "missing $SCRIPT_DIR/debdeps.py"
[[ "$DEB_ARCH" == arm64 ]] || fail "this script builds an arm64 rootfs (DEB_ARCH=$DEB_ARCH)"

write_if_changed() {   # mode dest  (content on stdin)
    local mode=$1 dest=$2 tmp
    tmp=$(mktemp)
    cat > "$tmp"
    if [[ -f "$dest" ]] && cmp -s "$tmp" "$dest"; then
        rm -f "$tmp"; return 0
    fi
    chmod "$mode" "$tmp"
    cp -f "$tmp" "$dest"
    rm -f "$tmp"
}

set_mode() {
    local want=$1 path=$2 have
    have=$(stat -c '%a' "$path")
    want="${want#"${want%%[!0]*}"}"
    [[ "$have" == "$want" ]] || chmod "$want" "$path"
}

symlink_if_changed() {
    [[ -L "$1" && "$(readlink "$1")" == "$2" ]] || ln -sfn "$2" "$1"
}

# Unpack one .deb into the rootfs, normalising merged-/usr first.
#
# Debian bookworm is a merged-/usr system: /bin, /sbin and /lib are symlinks into
# /usr. Individual packages are inconsistent: systemd and udev ship ./lib/udev
# while iproute2 ships ./usr/bin/ip. Extracting a ./lib/... member straight into
# the tree makes tar replace the /lib symlink with a real directory, which
# silently breaks every other path in the rootfs.
#
# So each archive is unpacked into a staging directory, its top-level bin/sbin/
# lib/lib64 are folded into usr/, and only then copied over the rootfs.
deb_unpack() {
    local deb=$1 stage root
    stage=$(mktemp -d)
    root="$stage/root"
    mkdir -p "$root"
    ( cd "$stage" && ar x "$deb" ) || { rm -rf "$stage"; return 1; }
    local data
    data=$(ls "$stage"/data.tar.* 2>/dev/null | head -n 1)
    [[ -n "$data" ]] || { rm -rf "$stage"; return 1; }
    tar -x -f "$data" -C "$root" || { rm -rf "$stage"; return 1; }

    local d entry name
    for d in bin sbin lib lib64 lib32 libx32; do
        # Only real directories need folding; a symlink is already merged.
        [[ -d "$root/$d" && ! -L "$root/$d" ]] || continue
        mkdir -p "$root/usr/$d"
        while IFS= read -r -d '' entry; do
            name=${entry#./}
            if [[ -L "$root/$d/$entry" ]]; then
                # Symlink: the archive's own /usr copy is canonical.
                rm -rf "$root/usr/$d/$name"
                cp -a "$root/$d/$entry" "$root/usr/$d/$name"
            elif [[ -d "$root/$d/$entry" ]]; then
                # Directory: merge contents, because the destination may already
                # exist with its own files. systemd for example ships both
                # ./lib/systemd/systemd (the binary) and ./usr/lib/systemd/system
                # (unit files); replacing instead of merging loses the binary.
                mkdir -p "$root/usr/$d/$name"
                cp -a "$root/$d/$entry/." "$root/usr/$d/$name/"
            else
                # Regular file: this archive is the one being installed, so its
                # version wins over anything already staged.
                rm -rf "$root/usr/$d/$name"
                cp -a "$root/$d/$entry" "$root/usr/$d/$name"
            fi
        done < <(cd "$root/$d" && find . -mindepth 1 -maxdepth 1 -print0)
        rm -rf "$root/$d"
    done

    cp -a "$root/." "$ROOTFS_DIR/"
    rm -rf "$stage"
}

# Fail if the rootfs's /bin, /sbin or /lib stopped being symlinks into /usr.
# That means a non-merged archive slipped through and the tree is inconsistent.
assert_merged_usr() {
    local d target
    for d in bin sbin lib; do
        [[ -e "$ROOTFS_DIR/$d" || -L "$ROOTFS_DIR/$d" ]] || continue
        if [[ ! -L "$ROOTFS_DIR/$d" ]]; then
            fail "/$d is a real directory, expected a symlink to usr/$d"
        fi
        target=$(readlink "$ROOTFS_DIR/$d")
        [[ "$target" == "usr/$d" ]] || fail "/$d points to '$target', expected 'usr/$d'"
    done
}

# ---------------------------------------------------------------------------
# 1. Base root filesystem
# ---------------------------------------------------------------------------
# Decide whether to (re-)extract the base. Re-extracting wipes the tree, so it
# must only happen when the tree is missing or incomplete. --keep forces reuse
# and fails loudly instead of silently rebuilding.
NEED_EXTRACT=0
if [[ "$KEEP" -eq 1 ]]; then
    if [[ ! -x "$ROOTFS_DIR/bin/bash" ]]; then
        fail "--keep was given but there is no rootfs at $ROOTFS_DIR"
    fi
elif [[ ! -x "$ROOTFS_DIR/bin/bash" || ! -d "$ROOTFS_DIR/etc" ]]; then
    NEED_EXTRACT=1
fi

if ((NEED_EXTRACT)); then
    if [[ ! -f "$BASE_TAR" ]]; then
        command -v podman >/dev/null 2>&1 \
            || fail "base tarball is missing and podman is unavailable: $BASE_TAR"
        note "exporting $BASE_IMAGE ($DEB_ARCH) with podman"
        mkdir -p "$BASE_DIR"
        podman pull --quiet --arch arm64 --os linux "$BASE_IMAGE" >/dev/null
        ctr=$(podman create "$BASE_IMAGE" 2>/dev/null)
        trap 'podman rm -f "$ctr" >/dev/null 2>&1 || true' EXIT
        podman export "$ctr" -o "$BASE_TAR" >/dev/null
        podman rm -f "$ctr" >/dev/null 2>&1 || true
        trap - EXIT
    fi
    note "verifying base rootfs image"
    printf '%s  %s\n' "$BASE_SHA256" "$BASE_TAR" | sha256sum --check --status - \
        || fail "base tarball SHA256 mismatch: $BASE_TAR (set BASE_SHA256 to accept a new base)"
    rm -rf "$ROOTFS_DIR"
    mkdir -p "$ROOTFS_DIR"
    note "extracting base rootfs"
    tar xf "$BASE_TAR" -C "$ROOTFS_DIR"
else
    note "reusing existing rootfs tree at $ROOTFS_DIR"
fi

[[ -x "$ROOTFS_DIR/bin/bash" ]] || fail "base rootfs has no /bin/bash"
if ! file -b "$ROOTFS_DIR/bin/bash" | grep -q aarch64; then
    fail "base /bin/bash is not aarch64: $(file -b "$ROOTFS_DIR/bin/bash")"
fi
note "base is aarch64: $(file -b "$ROOTFS_DIR/bin/bash" | cut -d, -f1-2)"

# The task's minimum package list. Fail loudly if the base lost one of these.
for p in bash coreutils util-linux e2fsprogs mount; do
    grep -q "^Package: $p$" "$ROOTFS_DIR/var/lib/dpkg/status" 2>/dev/null \
        || fail "base rootfs is missing the required package: $p"
done

# ---------------------------------------------------------------------------
# 2. Resolve, download and verify the package closure
# ---------------------------------------------------------------------------
mkdir -p "$BASE_DIR" "$DEB_CACHE/$DEB_DIST-$DEB_ARCH" "$KEY_DIR"
ARM64_INDEX="$BASE_DIR/$DEB_DIST-main-$DEB_ARCH-Packages.gz"
ALL_INDEX="$BASE_DIR/$DEB_DIST-main-all-Packages.gz"
for idx in "$ARM64_INDEX" "$ALL_INDEX"; do
    if [[ ! -s "$idx" ]]; then
        note "fetching package index $(basename "$idx")"
        curl --fail --location --retry 3 --silent --show-error \
            "$DEB_MIRROR/dists/$DEB_DIST/main/binary-$(basename "$idx" | sed -E 's/.*binary-(arm64|all)-Packages\.gz/\1/')/Packages.gz" \
            --output "$idx"
    fi
done

# Older cached indexes keep their original names; make them available under the
# names used above.
[[ -s "$ARM64_INDEX" ]] || cp -f "$BASE_DIR/bookworm-main-arm64-Packages.gz" "$ARM64_INDEX"
[[ -s "$ALL_INDEX"   ]] || cp -f "$BASE_DIR/bookworm-main-all-Packages.gz"   "$ALL_INDEX"

DEB_LIST=$(mktemp)
CACHE_TAG="$ROOTFS_DIR/.d2q-deb-packages"
trap 'rm -f "$DEB_LIST" "$RESOLVED" "$REAL_STATUS"' EXIT

note "resolving dependency closure for: ${PACKAGES[*]}"

# This script appends synthetic stanzas to the rootfs dpkg status when it unpacks
# a package, but it never runs maintainer scripts. Those stanzas would make the
# resolver believe the packages are already installed, so hide them from it and
# let the cache tag below decide what is actually present. Rebuilding the tag
# from the status file also keeps it self-healing if the tag is lost.
REAL_STATUS=$(mktemp)
awk -v RS='' -v ORS='\n\n' '!/unpacked by tools\/build-rootfs-arm64.sh/' \
    "$ROOTFS_DIR/var/lib/dpkg/status" > "$REAL_STATUS"
# Rebuild the tag from the status file, writing only if it actually changes.
TAG_NEW=$(mktemp)
awk -v RS='' '/unpacked by tools\/build-rootfs-arm64.sh/ && /^Package: /{print $2}' \
    "$ROOTFS_DIR/var/lib/dpkg/status" | grep -v '^$' | sort -u > "$TAG_NEW" || true
if ! cmp -s "$TAG_NEW" "$CACHE_TAG"; then
    cp -f "$TAG_NEW" "$CACHE_TAG"
    chmod 0644 "$CACHE_TAG"
fi
rm -f "$TAG_NEW"

RESOLVED=$(mktemp)
python3 "$SCRIPT_DIR/debdeps.py" \
    --index "$ARM64_INDEX" \
    --index "$ALL_INDEX" \
    --installed "$REAL_STATUS" \
    $(printf -- '--package %s ' "${PACKAGES[@]}") \
    --output "$RESOLVED"

# The previous run appended its packages to the rootfs dpkg status, so on a
# re-run the resolver correctly reports an empty closure. Filter out anything
# already recorded as unpacked so the two sources of truth cannot disagree.
DEB_LIST=$(mktemp)
if [[ -f "$CACHE_TAG" ]]; then
    while IFS=$'\t' read -r pkg version arch filename sha256 size; do
        grep -qx "$pkg" "$CACHE_TAG" || printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$pkg" "$version" "$arch" "$filename" "$sha256" "$size" >> "$DEB_LIST"
    done < "$RESOLVED"
else
    cp -f "$RESOLVED" "$DEB_LIST"
fi

wanted=$(grep -c . "$DEB_LIST" || true)
if [[ "$wanted" -eq 0 ]]; then
    note "all packages already present in the rootfs, nothing to unpack"
else
    note "downloading and verifying $wanted packages"
    while IFS=$'\t' read -r pkg version arch filename sha256 size; do
        [[ -n "$pkg" ]] || continue
        dest="$DEB_CACHE/$DEB_DIST-$DEB_ARCH/$(basename "$filename")"
        if [[ ! -f "$dest" ]]; then
            curl --fail --location --retry 3 --silent --show-error \
                "$DEB_MIRROR/$filename" --output "$dest" >&2
        fi
        # Never unpack a package that does not match the signed index.
        printf '%s  %s\n' "$sha256" "$dest" | sha256sum --check --status - \
            || fail "SHA256 mismatch for $pkg ($filename)"
    done < "$DEB_LIST"

    note "unpacking packages into the rootfs (archive extraction only)"
    while IFS=$'\t' read -r pkg version arch filename sha256 size; do
        [[ -n "$pkg" ]] || continue
        deb="$DEB_CACHE/$DEB_DIST-$DEB_ARCH/$(basename "$filename")"
        deb_unpack "$deb" || fail "could not unpack $pkg from $deb"
        assert_merged_usr
        # Record the package so later runs skip it and stay reproducible.
        {
            printf 'Package: %s\n' "$pkg"
            printf 'Status: install ok installed\n'
            printf 'Priority: optional\n'
            printf 'Section: misc\n'
            printf 'Installed-Size: 1\n'
            printf 'Maintainer: Debian <debian@lists.debian.org>\n'
            printf 'Architecture: %s\n' "$arch"
            printf 'Version: %s\n' "$version"
            printf 'Description: unpacked by tools/build-rootfs-arm64.sh for the headless d2q port\n'
            printf ' This package was unpacked on the host with ar+tar; no maintainer script ran.\n'
            printf '\n'
        } >> "$ROOTFS_DIR/var/lib/dpkg/status"
        grep -qx "$pkg" "$CACHE_TAG" 2>/dev/null || printf '%s\n' "$pkg" >> "$CACHE_TAG"
    done < "$DEB_LIST"
    sort -u "$CACHE_TAG" -o "$CACHE_TAG" 2>/dev/null || true
fi
rm -f "$RESOLVED"
total=$(grep -c . "$CACHE_TAG" || true)

# ---------------------------------------------------------------------------
# 3. Halium boot wiring
# ---------------------------------------------------------------------------
note "wiring the rootfs for the Halium initramfs"

# The initramfs execs `run-init ${rootmnt} /sbin/init`; systemd-sysv provides it.
[[ -e "$ROOTFS_DIR/sbin/init" ]] || fail "/sbin/init missing after unpacking systemd-sysv"
[[ -x "$ROOTFS_DIR/lib/systemd/systemd" ]] || fail "/lib/systemd/systemd missing"
[[ -x "$ROOTFS_DIR/lib/systemd/systemd-udevd" ]] || fail "systemd-udevd missing (udev is required)"
[[ -x "$ROOTFS_DIR/usr/bin/ip" || -x "$ROOTFS_DIR/sbin/ip" ]] || fail "iproute2 (ip) missing"
note "  /sbin/init -> $(readlink -f "$ROOTFS_DIR/sbin/init" | sed "s|$ROOTFS_DIR||")"

# The initramfs does `mount --move /android-rootfs ${rootmnt}/var/lib/lxc/android/rootfs`.
# mount(8) will not create the destination, so the parent must exist beforehand.
mkdir -p "$ROOTFS_DIR/var/lib/lxc/android"

# The initramfs moves its own /run and /dev into the new root.
for d in proc sys dev run tmp var/log; do
    [[ -d "$ROOTFS_DIR/$d" ]] || mkdir -p "$ROOTFS_DIR/$d"
done

# The initramfs remounts the rootfs read-only unless this marker exists. A
# headless box needs a writable /var (journald, sshd state, logs).
[[ -f "$ROOTFS_DIR/.writable_image" ]] || : > "$ROOTFS_DIR/.writable_image"
note "  created .writable_image (keeps the rootfs mounted read-write)"

# Boot straight to a multi-user system with networking; no display manager.
mkdir -p "$ROOTFS_DIR/etc"
# Default boot target. /etc/default/target is a unit alias, so this is a symlink.
symlink_if_changed "$ROOTFS_DIR/etc/default/target" "/lib/systemd/system/multi-user.target"

# The Halium initramfs bind-mounts its generated image.fstab over /etc/fstab only
# when /etc/system-image/writable-paths exists. A minimal /etc/fstab keeps
# systemd from touching anything the initramfs already mounted.
write_if_changed 0644 "$ROOTFS_DIR/etc/fstab" <<'EOF'
# Minimal headless rootfs. The Halium initramfs already mounted /proc, /sys,
# /dev and /run, and moved the userdata partition to /userdata. Everything is
# mounted by systemd itself, so no device entries are listed here.
#
# Do NOT add /etc/system-image/writable-paths: the initramfs would then replace
# this file with a generated bind-mount fstab.
EOF

# Serial console for early logs, and a getty is not needed (no display, no
# console on the device) but a kernel console keeps messages visible in dmesg.
mkdir -p "$ROOTFS_DIR/etc/systemd/system/serial-getty@ttyMSM0.service.d" 2>/dev/null || true

# ---------------------------------------------------------------------------
# 4. SSH
# ---------------------------------------------------------------------------
SSH_ETC="$ROOTFS_DIR/etc/ssh"
mkdir -p "$SSH_ETC" "$ROOTFS_DIR/root/.ssh"
set_mode 0700 "$ROOTFS_DIR/root/.ssh"

if [[ ! -f "$KEY_DIR/d2q_rsa" ]]; then
    note "generating client key $KEY_DIR/d2q_rsa"
    ssh-keygen -q -t rsa -b 3072 -N "" -C "d2q-halium-headless" -f "$KEY_DIR/d2q_rsa"
fi
write_if_changed 0600 "$ROOTFS_DIR/root/.ssh/authorized_keys" < "$KEY_DIR/d2q_rsa.pub"
set_mode 0600 "$ROOTFS_DIR/root/.ssh/authorized_keys"

HOSTKEY_MARKER="$SSH_ETC/.d2q-hostkeys-generated"
# Host keys are generated once into artifacts/keys and then installed, not
# generated inside the rootfs. Two reasons: the image stays reproducible, and a
# rebuild does not silently change the device's SSH identity, which would make
# every client see a changed host key.
ensure_host_key() {
    local algo=$1 src="$KEY_DIR/ssh_host_${algo}_key"
    if [[ ! -f "$src" ]]; then
        ssh-keygen -q -t "$algo" -N "" -C "d2q-headless-host" -f "$src"
        note "  generated $src"
    fi
}
for algo in rsa ecdsa ed25519; do
    ensure_host_key "$algo"
    write_if_changed 0600 "$SSH_ETC/ssh_host_${algo}_key" < "$KEY_DIR/ssh_host_${algo}_key"
    write_if_changed 0644 "$SSH_ETC/ssh_host_${algo}_key.pub" < "$KEY_DIR/ssh_host_${algo}_key.pub"
done
[[ -f "$HOSTKEY_MARKER" ]] || : > "$HOSTKEY_MARKER"

write_if_changed 0644 "$SSH_ETC/sshd_config" <<EOF
# Headless d2q SSH server. Managed by tools/build-rootfs-arm64.sh.
Port 22
AddressFamily any
ListenAddress 0.0.0.0
ListenAddress ::

# Key-based root login only. The reference rootfs has no root password and a
# headless device has no console to set one into.
PermitRootLogin prohibit-password
PubkeyAuthentication yes
AuthorizedKeysFile .ssh/authorized_keys
PasswordAuthentication no
KbdInteractiveAuthentication no

UsePAM no
X11Forwarding no
PrintMotd no
AcceptEnv LANG LC_*

# Modern algorithms only. ssh-rsa (SHA-1) and ssh-dss are rejected by modern
# OpenSSH clients, and rsa-sha2-* is what the pinned client key relies on.
HostKeyAlgorithms ssh-ed25519,rsa-sha2-512,rsa-sha2-256,ecdsa-sha2-nistp256
PubkeyAcceptedAlgorithms ssh-ed25519,rsa-sha2-512,rsa-sha2-256,ecdsa-sha2-nistp256

Subsystem sftp /usr/lib/openssh/sftp-server
EOF
note "  wrote sshd_config with rsa-sha2 support"

# ---------------------------------------------------------------------------
# 5. USB RNDIS
# ---------------------------------------------------------------------------
write_if_changed 0755 "$ROOTFS_DIR/usr/bin/d2q-rndis" < "$SCRIPT_DIR/d2q-rndis.sh"
write_if_changed 0644 "$ROOTFS_DIR/lib/systemd/system/d2q-rndis.service" <<EOF
# USB RNDIS gadget for the d2q headless port. Managed by
# tools/build-rootfs-arm64.sh. Uses the kernel's configfs gadget API.
[Unit]
Description=d2q USB RNDIS gadget (configfs)
After=systemd-modules-load.service local-fs.target systemd-udevd.service
Wants=systemd-modules-load.service
ConditionPathExists=/sys/kernel/config/usb_gadget

[Service]
Type=oneshot
RemainAfterExit=yes
Environment=DEVICE_IP=$DEVICE_IP
Environment=PREFIX=$DEVICE_PREFIX
ExecStart=/usr/bin/d2q-rndis
ExecStop=/usr/bin/d2q-rndis --cleanup

[Install]
WantedBy=multi-user.target
EOF

write_if_changed 0644 "$ROOTFS_DIR/etc/systemd/network/10-d2q-rndis.network" <<EOF
# Static USB RNDIS link for the d2q headless port.
# The host end of the link is $HOST_IP/$HOST_PREFIX (see PORTING-STATUS.md).
[Match]
Name=usb0 rndis0 enx* usb*

[Link]
# Deterministic MAC so the host can recognise the device.
MACAddress=02:11:22:33:44:55

[Network]
Address=$DEVICE_IP/$DEVICE_PREFIX
DHCP=no
# Point-to-point link: the host is the only peer, so no gateway.
Gateway=
DNS=
LinkLocalAddressing=no
IPv6AcceptRA=no
IPv6PrivacyExtensions=no
EOF

mkdir -p "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants" \
         "$ROOTFS_DIR/etc/systemd/system/sockets.target.wants"
# These symlink targets must be paths *inside the rootfs*, exactly as
# `systemctl enable` would write them, not host paths.
symlink_if_changed "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants/d2q-rndis.service" \
    "/lib/systemd/system/d2q-rndis.service"
symlink_if_changed "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants/systemd-networkd.service" \
    "/lib/systemd/system/systemd-networkd.service"
symlink_if_changed "$ROOTFS_DIR/etc/systemd/system/sockets.target.wants/systemd-networkd.socket" \
    "/lib/systemd/system/systemd-networkd.socket"
note "  device $DEVICE_IP/$DEVICE_PREFIX, host $HOST_IP/$HOST_PREFIX, interface discovered at runtime"

# ---------------------------------------------------------------------------
# 6. Keep it headless
# ---------------------------------------------------------------------------
# The base image is a Docker image, so strip the container-specific bits that
# would confuse systemd on a real device.
rm -f "$ROOTFS_DIR/.dockerenv"
rm -rf "$ROOTFS_DIR/etc/apt/apt.conf.d/docker*"
# Do not rm resolv.conf first: write_if_changed compares against the existing
# file, and unlinking it would force a rewrite (and bump /etc's mtime) on every
# run, which changes the packed image hash.
write_if_changed 0644 "$ROOTFS_DIR/etc/resolv.conf" <<'EOF'
# Intentionally empty: the USB link is point-to-point with the host and needs
# no resolver. A systemd-resolved stub would only add a failure mode here.
nameserver 127.0.0.1
options edns0 trust-ad
EOF
rm -rf "$ROOTFS_DIR/var/lib/apt/lists" "$ROOTFS_DIR/var/cache/apt" "$ROOTFS_DIR/var/log/apt"
# There is no system.img, so no LXC container and no ofono can ever work here.
# Nothing in this rootfs enables them; mask them anyway so a future image
# cannot accidentally pull them in.
for unit in lxc@.service lxc.service ofono.service dundee.service \
            NetworkManager.service systemd-resolved.service; do
    if [[ -e "$ROOTFS_DIR/lib/systemd/system/$unit" ]]; then
        symlink_if_changed /dev/null "$ROOTFS_DIR/etc/systemd/system/$unit"
    fi
done
# No graphical or Android user interface is installed; make that explicit for
# anyone inspecting the image.
mkdir -p "$ROOTFS_DIR/etc"
write_if_changed 0644 "$ROOTFS_DIR/etc/d2q-headless" <<EOF
# This is a headless d2q Halium rootfs.
# It contains no Android system image, no LXC container and no graphical stack.
# Boot target: kernel -> Halium initramfs -> this rootfs -> USB RNDIS -> SSH.
# Device address: $DEVICE_IP/$DEVICE_PREFIX
# Host address:   $HOST_IP/$HOST_PREFIX
# Rebuild with:   tools/build-rootfs-arm64.sh
EOF

# ---------------------------------------------------------------------------
# 8. Normalise timestamps
# ---------------------------------------------------------------------------
# mke2fs -d records each file's mtime, so a rootfs built now and the same rootfs
# built tomorrow would pack to different image hashes. Pin every timestamp so
# the image is reproducible from scratch, not only when re-run in place.
# The file list is sorted so the traversal order is stable too.
SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1767225600}   # 2026-01-01 00:00:00 UTC
if [[ "${NORMALIZE_TIMES:-1}" == "1" ]]; then
    note "normalising timestamps to $SOURCE_DATE_EPOCH"
    # Directories first (deepest last) so touching a file does not re-dirty a
    # parent directory, then symlinks, then regular files.
    find "$ROOTFS_DIR" -depth -type d -print0 \
        | xargs -0 -r touch -h -d "@$SOURCE_DATE_EPOCH"
    find "$ROOTFS_DIR" -type l -print0 \
        | xargs -0 -r touch -h -d "@$SOURCE_DATE_EPOCH"
    find "$ROOTFS_DIR" ! -type d ! -type l -print0 \
        | xargs -0 -r touch -h -d "@$SOURCE_DATE_EPOCH"
fi

# ---------------------------------------------------------------------------
# 9. Verify
# ---------------------------------------------------------------------------
note "verifying the prepared ARM64 rootfs"
check_exec() { [[ -x "$ROOTFS_DIR/$1" ]] || fail "missing or not executable: /$1"; }
# Locations follow Debian merged-/usr: kmod installs into /sbin, iproute2 into
# /bin, and /sbin and /bin are symlinks into /usr, so both spellings work.
check_exec bin/bash
check_exec lib/systemd/systemd
check_exec sbin/init
check_exec usr/sbin/sshd
check_exec usr/bin/ip
check_exec sbin/modprobe
check_exec sbin/lsmod
check_exec usr/bin/d2q-rndis
check_exec usr/lib/systemd/systemd-udevd
check_exec usr/bin/udevadm

# Every shared library the SSH daemon needs must resolve inside the rootfs.
missing_libs=0
for bin in usr/sbin/sshd usr/bin/ip lib/systemd/systemd; do
    while read -r so; do
        so=${so#(}
        so=${so%)}
        so=$(printf '%s' "$so" | tr -d ' ')
        [[ -z "$so" ]] && continue
        if ! find "$ROOTFS_DIR/lib" "$ROOTFS_DIR/usr/lib" -maxdepth 3 -name "$so*" \
             -print -quit 2>/dev/null | grep -q .; then
            printf '    MISSING library for /%s: %s\n' "$bin" "$so" >&2
            missing_libs=$((missing_libs + 1))
        fi
    done < <(python3 - "$ROOTFS_DIR/$bin" <<'PY'
import re, sys
data = open(sys.argv[1], 'rb').read()
for name in sorted(set(re.findall(rb'lib[a-zA-Z0-9_.+-]*\.so[.0-9]*', data))):
    print(name.decode())
PY
)
done
[[ "$missing_libs" -eq 0 ]] || fail "$missing_libs shared libraries are unresolved in the rootfs"

[[ -d "$ROOTFS_DIR/var/lib/lxc/android" ]] || fail "/var/lib/lxc/android missing (initramfs mount --move target)"
[[ -f "$ROOTFS_DIR/.writable_image" ]] || fail ".writable_image missing (rootfs would boot read-only)"

printf '==> ARM64 headless rootfs ready. No device operation was performed.\n'
printf '    rootfs    : %s\n' "$ROOTFS_DIR"
printf '    packages  : %s unpacked and checksum-verified\n' "$total"
printf '    device IP : %s/%s\n' "$DEVICE_IP" "$DEVICE_PREFIX"
printf '    host IP   : %s/%s\n' "$HOST_IP" "$HOST_PREFIX"
printf '    client key: %s\n' "$KEY_DIR/d2q_rsa"
printf '    on host   : ssh -i %s root@%s\n' "$KEY_DIR/d2q_rsa" "$DEVICE_IP"
