#!/usr/bin/env bash
# Prepare an extracted Halium/Debian rootfs for headless d2q use over SSH.
#
# This script is deliberately host-side only. It never calls adb, fastboot,
# Odin, Heimdall, or any flashing tool, and it never repartitions anything.
#
# It performs, in order:
#   1. Adds openssh-server (Debian buster armhf) by unpacking signed-by-index
#      .deb archives with dpkg-deb -x. No qemu, no root, no maintainer scripts.
#   2. Installs a pinned root authorized_keys so `ssh root@<ip>` is key-based.
#   3. Writes a headless sshd_config and generates host keys if absent.
#   4. Installs the d2q configfs RNDIS setup as a systemd unit so the device
#      always comes up on the documented 10.15.19.82/24 address.
#   5. Optionally masks Android-dependent units that cannot work without a
#      Halium system image (--no-android, default for a headless box).
#
# Re-running is safe: every step checks for its own result first.
set -Eeuo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

ROOTFS_DIR=${ROOTFS_DIR:-"$ROOT/artifacts/generated/halium-rootfs"}
ARTIFACTS=${ARTIFACTS:-"$ROOT/artifacts"}
KEY_DIR=${KEY_DIR:-"$ARTIFACTS/keys"}
DEVICE_IP=${DEVICE_IP:-10.15.19.82}
DEVICE_PREFIX=${DEVICE_PREFIX:-24}
DEB_MIRROR=${DEB_MIRROR:-http://archive.debian.org/debian}
DEB_DIST=${DEB_DIST:-buster}
DEB_ARCH=${DEB_ARCH:-armhf}
MASK_ANDROID=1

usage() {
    cat <<'EOF'
Usage: tools/prepare-rootfs.sh [--rootfs DIR] [--no-mask-android] [--keep-android]

Prepares an already-extracted rootfs for headless SSH use. Expects the
rootfs directory to have been produced by extracting the Halium Debian
rootfs tarball:

    tar xzf halium-debian-rootfs_<date>.tar.gz -C <dir>

Device nodes under dev/ are not created when running unprivileged; that is
harmless because the kernel's devtmpfs provides them at boot.

Options:
  --rootfs DIR        rootfs directory to prepare (default: artifacts/generated/halium-rootfs)
  --keep-android      keep lxc@android/ofono/usb-tethering units enabled
  --no-mask-android   alias for --keep-android (masking is the default)
  -h, --help          show this help

This script NEVER flashes and NEVER contacts a device.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --rootfs) ROOTFS_DIR=$2; shift 2 ;;
        --keep-android|--no-mask-android) MASK_ANDROID=0; shift ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'error: unknown argument: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done

fail() { printf 'error: %s\n' "$*" >&2; exit 1; }
note() { printf '==> %s\n' "$*"; }

# Write a file only when its content would actually change.
#
# mke2fs -d records each file's mtime, so rewriting an identical file on every
# run would make rootfs.img non-reproducible. Skipping no-op writes keeps the
# whole pipeline byte-for-byte stable.
write_if_changed() {
    local mode=$1 dest=$2
    # Stage outside the rootfs: creating and renaming a temp file inside a
    # target directory would bump that directory's mtime, and mke2fs -d records
    # directory mtimes too.
    local tmp
    tmp=$(mktemp) || fail "cannot create temp file"
    cat > "$tmp"
    if [[ -f "$dest" ]] && cmp -s "$tmp" "$dest"; then
        rm -f "$tmp"
        return 0
    fi
    chmod "$mode" "$tmp"
    cp -f "$tmp" "$dest"
    rm -f "$tmp"
}

# Create a symlink only when it is missing or points somewhere else, so repeated
# runs do not churn the mtime.
symlink_if_changed() {
    local target=$1 dest=$2
    if [[ -L "$dest" && "$(readlink "$dest")" == "$target" ]]; then
        return 0
    fi
    ln -sfn "$target" "$dest"
}

[[ -d "$ROOTFS_DIR" ]] || fail "rootfs directory not found: $ROOTFS_DIR"
[[ -x "$ROOTFS_DIR/bin/sh" ]] || fail "$ROOTFS_DIR does not look like a rootfs (no /bin/sh)"
for cmd in curl tar ssh-keygen; do
    command -v "$cmd" >/dev/null 2>&1 || fail "required host command is missing: $cmd"
done

# A .deb is an ar(1) archive wrapping a data tarball. Use dpkg-deb when the host
# is Debian-family, otherwise fall back to bsdtar or ar+tar so this script also
# works on Arch/CachyOS hosts. Extraction only: no maintainer scripts run.
DEB_EXTRACTOR=""
deb_extract() {
    local deb=$1 dest=$2
    case "$DEB_EXTRACTOR" in
        dpkg-deb) dpkg-deb -x "$deb" "$dest"; return ;;
        ar)       ar_extract "$deb" "$dest"; return ;;
        bsdtar)   bsdtar -x -f "$deb" -C "$dest"; return ;;
    esac
    if command -v dpkg-deb >/dev/null 2>&1; then
        DEB_EXTRACTOR=dpkg-deb
        dpkg-deb -x "$deb" "$dest"
    elif command -v ar >/dev/null 2>&1; then
        DEB_EXTRACTOR=ar
        ar_extract "$deb" "$dest"
    elif command -v bsdtar >/dev/null 2>&1; then
        DEB_EXTRACTOR=bsdtar
        bsdtar -x -f "$deb" -C "$dest"
    else
        fail "no .deb extractor available (need dpkg-deb, ar, or bsdtar)"
    fi
}

# Portable fallback: a .deb is an ar(1) archive whose data member is a tarball.
ar_extract() {
    local deb=$1 dest=$2
    local tmp data
    tmp=$(mktemp -d)
    ( cd "$tmp" && ar x "$deb" )
    data=$(ls "$tmp"/data.tar.* 2>/dev/null | head -n 1)
    if [[ -z "$data" ]]; then
        rm -rf "$tmp"
        printf 'no data.tar.* member in %s\n' "$deb" >&2
        return 1
    fi
    tar -x -f "$data" -C "$dest"
    rm -rf "$tmp"
}

# Confirm the rootfs architecture before unpacking anything into it.
BUSYBOX=$ROOTFS_DIR/bin/busybox
if [[ -f "$BUSYBOX" ]]; then
    ARCH_STR=$(file -b "$BUSYBOX" 2>/dev/null || echo unknown)
    case "$ARCH_STR" in
        *aarch64*|*ARM64*) ROOTFS_ARCH=arm64 ;;
        *ARM*) ROOTFS_ARCH=armhf ;;
        *) ROOTFS_ARCH=unknown ;;
    esac
    note "rootfs architecture: $ROOTFS_ARCH (detected from bin/busybox)"
    if [[ "$ROOTFS_ARCH" == armhf && "$DEB_ARCH" == armhf ]]; then
        note "WARNING: this is a 32-bit armhf rootfs."
        note "         The d2q kernel has CONFIG_COMPAT=y so armhf binaries run,"
        note "         but the supported Halium combination is arm64 rootfs + arm64 system.img."
    fi
    if [[ "$ROOTFS_ARCH" == arm64 && "$DEB_ARCH" != arm64 ]]; then
        fail "rootfs is arm64 but DEB_ARCH is $DEB_ARCH; set DEB_ARCH=arm64"
    fi
fi

DEB_CACHE="$ARTIFACTS/debs/$DEB_DIST-$DEB_ARCH"
mkdir -p "$DEB_CACHE" "$KEY_DIR"
PACKAGES_INDEX="$DEB_CACHE/Packages"

# ---------------------------------------------------------------------------
# 1. Fetch the package index once and verify every .deb we unpack.
# ---------------------------------------------------------------------------
note "fetching package index for $DEB_DIST/$DEB_ARCH"
if [[ ! -s "$PACKAGES_INDEX" ]]; then
    curl --fail --location --retry 3 --silent --show-error \
        "$DEB_MIRROR/dists/$DEB_DIST/main/binary-$DEB_ARCH/Packages.gz" \
        --output "$DEB_CACHE/Packages.gz"
    gzip -dc "$DEB_CACHE/Packages.gz" > "$PACKAGES_INDEX"
fi
[[ -s "$PACKAGES_INDEX" ]] || fail "package index is empty: $PACKAGES_INDEX"

# index_field <package> <field>
index_field() {
    awk -v want="$1" -v field="$2" '
        /^Package: /   { inpkg = ($2 == want) }
        inpkg && $1 == field":" { print $2; exit }
    ' "$PACKAGES_INDEX"
}

fetch_deb() {
    local pkg=$1
    local filename sha256 dest
    filename=$(index_field "$pkg" Filename) || true
    sha256=$(index_field "$pkg" SHA256) || true
    [[ -n "$filename" ]] || fail "package $pkg not found in $DEB_DIST/$DEB_ARCH index"
    [[ -n "$sha256" ]] || fail "no SHA256 for $pkg in index (cannot verify)"

    dest="$DEB_CACHE/$(basename "$filename")"
    if [[ ! -f "$dest" ]]; then
        note "downloading $pkg" >&2
        # Progress and diagnostics must not pollute stdout, which carries the
        # resulting path to the caller.
        curl --fail --location --retry 3 --silent --show-error "$DEB_MIRROR/$filename" \
            --output "$dest" >&2
    fi
    printf '%s  %s\n' "$sha256" "$dest" | sha256sum --check --status - \
        || fail "SHA256 mismatch for $pkg ($dest)"
    printf '%s\n' "$dest"
}

# ---------------------------------------------------------------------------
# 2. Install openssh-server plus any dependency missing from the rootfs.
# ---------------------------------------------------------------------------
SSH_PKGS=(openssh-server openssh-client openssh-sftp-server)
for extra in libwrap0 ucf libcom-err2; do
    if ! grep -q "^Package: $extra$" "$ROOTFS_DIR/var/lib/dpkg/status" 2>/dev/null; then
        SSH_PKGS+=("$extra")
    fi
done

note "installing openssh-server into the rootfs (archive extraction, no qemu, no maintainer scripts)"
# Track what has been unpacked so re-runs do not rewrite files in the tree;
# mke2fs -d records mtimes and rewriting them would break reproducibility.
UNPACK_MARKER="$ROOTFS_DIR/.d2q-unpacked-packages"
# touch only when absent: it would otherwise bump the mtime on every run.
[[ -f "$UNPACK_MARKER" ]] || : > "$UNPACK_MARKER"

for pkg in "${SSH_PKGS[@]}"; do
    if grep -qx "$pkg" "$UNPACK_MARKER"; then
        note "  $pkg already unpacked, skipping"
        continue
    fi
    deb=$(fetch_deb "$pkg")
    version=$(index_field "$pkg" Version)
    if deb_extract "$deb" "$ROOTFS_DIR"; then
        note "  unpacked $pkg $version"
        grep -qx "$pkg" "$UNPACK_MARKER" || printf '%s\n' "$pkg" >> "$UNPACK_MARKER"
    else
        fail "could not unpack $pkg from $deb"
    fi
done

# Record the packages in the rootfs dpkg database so later in-device apt/dpkg
# does not think they are missing. Appended stanzas, never rewritten in place.
DPDB="$ROOTFS_DIR/var/lib/dpkg/status"
for pkg in "${SSH_PKGS[@]}"; do
    grep -q "^Package: $pkg$" "$DPDB" 2>/dev/null && continue
    version=$(index_field "$pkg" Version)
    arch=$(index_field "$pkg" Architecture)
    {
        printf '\nPackage: %s\n' "$pkg"
        printf 'Status: install ok installed\n'
        printf 'Priority: optional\n'
        printf 'Section: net\n'
        printf 'Installed-Size: 1\n'
        printf 'Maintainer: Debian OpenSSH Maintainers <debian-ssh@lists.debian.org>\n'
        printf 'Architecture: %s\n' "${arch:-$DEB_ARCH}"
        printf 'Version: %s\n' "$version"
        printf 'Description: unpacked by tools/prepare-rootfs.sh for headless SSH\n'
        printf ' This package was unpacked on the host with dpkg-deb -x.\n'
    } >> "$DPDB"
done

[[ -x "$ROOTFS_DIR/usr/sbin/sshd" ]] || fail "sshd missing after unpack"

# ---------------------------------------------------------------------------
# 3. Host keys and pinned root key.
# ---------------------------------------------------------------------------
SSH_ETC="$ROOTFS_DIR/etc/ssh"
mkdir -p "$SSH_ETC"
install -d -m 0700 "$ROOTFS_DIR/root/.ssh"

if [[ ! -f "$KEY_DIR/d2q_rsa" ]]; then
    note "generating client key $KEY_DIR/d2q_rsa"
    ssh-keygen -q -t rsa -b 3072 -N "" -C "d2q-halium-headless" -f "$KEY_DIR/d2q_rsa"
fi
write_if_changed 0600 "$ROOTFS_DIR/root/.ssh/authorized_keys" < "$KEY_DIR/d2q_rsa.pub"
note "installed root authorized_keys (ssh-rsa, from $KEY_DIR/d2q_rsa.pub)"

# Host keys are generated once on the host so the image is reproducible.
# The 2017 reference rootfs ships its own dropbear host keys; those are not
# secret and are replaced on the first run, then left alone.
HOSTKEY_MARKER="$SSH_ETC/.d2q-hostkeys-generated"
if [[ ! -f "$HOSTKEY_MARKER" ]]; then
    rm -f "$SSH_ETC"/ssh_host_*_key "$SSH_ETC"/ssh_host_*_key.pub
    for algo in rsa ecdsa ed25519; do
        ssh-keygen -q -t "$algo" -N "" -C "d2q-headless-host" \
            -f "$SSH_ETC/ssh_host_${algo}_key" >/dev/null
    done
    : > "$HOSTKEY_MARKER"
    note "generated host keys (rsa, ecdsa, ed25519)"
else
    note "reusing existing host keys (delete $HOSTKEY_MARKER to regenerate)"
fi
# Apply the expected modes only when they are not already correct. chmod bumps
# ctime, which mke2fs -d records, so an unconditional chmod on every run would
# change the image hash.
set_mode() {
    local want=$1 path=$2 have
    # stat -c %a prints without a leading zero ("600"), so normalise both sides
    # before comparing or the condition is never true.
    have=$(stat -c '%a' "$path")
    want="${want#"${want%%[!0]*}"}"   # strip leading zeros
    [[ "$have" == "$want" ]] || chmod "$want" "$path"
}
set_mode 0600 "$SSH_ETC/ssh_host_rsa_key"
set_mode 0600 "$SSH_ETC/ssh_host_ecdsa_key"
set_mode 0600 "$SSH_ETC/ssh_host_ed25519_key"
set_mode 0644 "$SSH_ETC/ssh_host_rsa_key.pub"
set_mode 0644 "$SSH_ETC/ssh_host_ecdsa_key.pub"
set_mode 0644 "$SSH_ETC/ssh_host_ed25519_key.pub"

# ---------------------------------------------------------------------------
# 4. Headless sshd configuration.
# ---------------------------------------------------------------------------
write_if_changed 0644 "$SSH_ETC/sshd_config" <<EOF
# Headless d2q Halium SSH server. Managed by tools/prepare-rootfs.sh.
Port 22
AddressFamily any
ListenAddress 0.0.0.0
ListenAddress ::

# Key-based root login only: the reference rootfs ships no root password and
# a headless device has no console to type one into.
PermitRootLogin prohibit-password
PubkeyAuthentication yes
AuthorizedKeysFile .ssh/authorized_keys

# Keep the daemon quiet and dependency-free on a device with no console.
UsePAM no
X11Forwarding no
PrintMotd no
AcceptEnv LANG LC_*

# Modern, still-supported algorithms. Deliberately no ssh-rsa (SHA-1) and no
# ssh-dss, which current OpenSSH clients reject by default.
HostKeyAlgorithms ssh-ed25519,rsa-sha2-512,rsa-sha2-256,ecdsa-sha2-nistp256
PubkeyAcceptedAlgorithms ssh-ed25519,rsa-sha2-512,rsa-sha2-256,ecdsa-sha2-nistp256

Subsystem sftp /usr/lib/openssh/sftp-server
EOF
note "wrote headless sshd_config"

# sshd refuses to start with a bad config; validate it if a host ssh binary
# of the same vintage is unavailable we can only do a structural check.
if command -v sshd >/dev/null 2>&1; then
    sshd -t -f "$SSH_ETC/sshd_config" 2>/dev/null \
        && note "sshd_config validated by host sshd" \
        || note "host sshd could not validate this config (expected for an armhf rootfs); skipping"
else
    note "host has no sshd; sshd_config not validated locally"
fi

# ---------------------------------------------------------------------------
# 5. USB RNDIS gadget as a systemd unit.
# ---------------------------------------------------------------------------
write_if_changed 0755 "$ROOTFS_DIR/usr/bin/d2q-rndis" < "$SCRIPT_DIR/d2q-rndis.sh"
write_if_changed 0644 "$ROOTFS_DIR/lib/systemd/system/d2q-rndis.service" <<EOF
# USB RNDIS gadget for the d2q headless port. Managed by tools/prepare-rootfs.sh.
[Unit]
Description=d2q USB RNDIS gadget (configfs)
Documentation=https://docs.halium.org/
# configfs is mounted by systemd; make sure the kernel exposes it first.
After=systemd-modules-load.service local-fs.target
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

# systemd-networkd owns the interface address, so the setup script only has to
# create the gadget; keep the address in one place per component.
mkdir -p "$ROOTFS_DIR/etc/systemd/network"
write_if_changed 0644 "$ROOTFS_DIR/etc/systemd/network/10-d2q-rndis.network" <<EOF
# Static USB RNDIS link for the d2q headless port.
[Match]
Name=usb0 rndis0 enx*

[Network]
Address=$DEVICE_IP/$DEVICE_PREFIX
# No gateway: the host is the only peer on this point-to-point link.
DHCP=no
LinkLocalAddressing=no
IPv6AcceptRA=no
EOF
note "installed d2q-rndis.service and 10-d2q-rndis.network ($DEVICE_IP/$DEVICE_PREFIX)"

mkdir -p "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants" \
         "$ROOTFS_DIR/etc/systemd/system/sockets.target.wants"
symlink_if_changed /lib/systemd/system/d2q-rndis.service \
    "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants/d2q-rndis.service"
symlink_if_changed /lib/systemd/system/systemd-networkd.service \
    "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants/systemd-networkd.service"
symlink_if_changed /lib/systemd/system/systemd-networkd.socket \
    "$ROOTFS_DIR/etc/systemd/system/sockets.target.wants/systemd-networkd.socket"

# Drop the reference rootfs's NetworkManager: it would fight systemd-networkd
# for the RNDIS interface and rename it unpredictably.
rm -f "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants/NetworkManager.service"

# The reference rootfs's usb-tethering unit drives Android's legacy USB gadget
# (needs /sys/class/android_usb/android0), which does not exist in a headless
# native Linux boot. d2q-rndis replaces it.
rm -f "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants/usb-tethering.service"

# Dropbear would fight OpenSSH for port 22.
rm -f "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants/dropbear.service" 2>/dev/null || true

# ---------------------------------------------------------------------------
# 6. Mask Android-dependent units when asked for a pure headless box.
# ---------------------------------------------------------------------------
if [[ "$MASK_ANDROID" -eq 1 ]]; then
    note "masking Android-dependent units (use --keep-android to retain them)"
    for unit in lxc@.service lxc.service ofono.service dundee.service; do
        rm -f "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants/$unit"
        [[ -e "$ROOTFS_DIR/lib/systemd/system/$unit" ]] || continue
        # A unit with no [Install] section cannot be re-enabled in place, so
        # mask it with a symlink to /dev/null instead.
        rm -rf "$ROOTFS_DIR/etc/systemd/system/$unit.d"
        symlink_if_changed /dev/null "$ROOTFS_DIR/etc/systemd/system/$unit"
        note "  masked $unit"
    done
    # lxc-net is only meaningful with a running android container.
    rm -f "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants/lxc-net.service"
    # The Halium reference rootfs also enables these; they are Android-side.
    rm -f "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants/lxc@android.service"
fi

# ---------------------------------------------------------------------------
# 7. Verify.
# ---------------------------------------------------------------------------
note "verifying prepared rootfs"
[[ -x "$ROOTFS_DIR/usr/sbin/sshd" ]]              || fail "sshd not executable"
[[ -s "$ROOTFS_DIR/root/.ssh/authorized_keys" ]]   || fail "root authorized_keys missing"
[[ -s "$ROOTFS_DIR/etc/ssh/sshd_config" ]]         || fail "sshd_config missing"
[[ -x "$ROOTFS_DIR/usr/bin/d2q-rndis" ]]           || fail "d2q-rndis not installed"
[[ -s "$ROOTFS_DIR/lib/systemd/system/d2q-rndis.service" ]] || fail "rndis unit missing"
[[ -s "$ROOTFS_DIR/etc/systemd/network/10-d2q-rndis.network" ]] || fail "network file missing"
for k in rsa ecdsa ed25519; do
    [[ -s "$ROOTFS_DIR/etc/ssh/ssh_host_${k}_key" ]] || fail "missing host key: $k"
done

printf '==> Rootfs prepared. No device operation was performed.\n'
printf '    rootfs    : %s\n' "$ROOTFS_DIR"
printf '    device IP : %s/%s\n' "$DEVICE_IP" "$DEVICE_PREFIX"
printf '    client key: %s\n' "$KEY_DIR/d2q_rsa"
printf '    on host   : ssh -i %s root@%s\n' "$KEY_DIR/d2q_rsa" "$DEVICE_IP"
