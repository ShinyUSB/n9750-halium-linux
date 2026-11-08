#!/usr/bin/env bash
# Audit a d2q kernel config without changing the baseline defconfig.
set -Eeuo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
KERNEL_DIR=${KERNEL_DIR:-"$ROOT/ShinyKernel/Kernel"}
CONFIG=${1:-"$KERNEL_DIR/out/.config"}
HALIUM_CHECK=${HALIUM_CHECK:-"$ROOT/n9750-halium/bootable/halium-boot/check-kernel-config"}
WRITE=0

if [[ "${2:-}" == "--write" ]]; then
    WRITE=1
fi

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

[[ -f "$CONFIG" ]] || fail "config not found: $CONFIG"

printf '==> Kernel: %s\n' "$KERNEL_DIR"
printf '==> Config:  %s\n' "$CONFIG"

required=(
    CONFIG_USB_GADGET
    CONFIG_USB_CONFIGFS
    CONFIG_USB_CONFIGFS_RNDIS
    CONFIG_USB_F_RNDIS
    CONFIG_USB_CONFIGFS_F_FS
    CONFIG_DEVTMPFS
    CONFIG_DEVTMPFS_MOUNT
    CONFIG_NAMESPACES
    CONFIG_UTS_NS
    CONFIG_NET_NS
    CONFIG_IPC_NS
    CONFIG_USER_NS
    CONFIG_PID_NS
    CONFIG_CGROUPS
    CONFIG_BLK_DEV_INITRD
    CONFIG_RD_GZIP
    CONFIG_INET
    CONFIG_IPV6
)

if ((WRITE)); then
    work=$(mktemp)
    trap 'rm -f "$work"' EXIT
    cp "$CONFIG" "$work"
else
    work="$CONFIG"
fi

missing=0
for symbol in "${required[@]}"; do
    if grep -qE "^${symbol}=y$" "$work"; then
        printf 'ok       %s=y\n' "$symbol"
    else
        printf 'MISSING  %s\n' "$symbol"
        missing=$((missing + 1))
    fi
done

if [[ -x "$HALIUM_CHECK" ]]; then
    report=$(mktemp)
    # Only ever remove the scratch copies. Without --write, $work *is* the
    # caller's config file and must not be deleted.
    cleanup() { rm -f "$report"; ((WRITE)) && rm -f "$work"; return 0; }
    trap cleanup EXIT
    if ((WRITE)); then
        "$HALIUM_CHECK" "$work" -w >"$report" 2>&1
    else
        "$HALIUM_CHECK" "$work" >"$report" 2>&1
    fi
    printf '\n==> Upstream Halium check output:\n'
    sed -n '/Config file checked/,$p' "$report"
    if grep -q 'found no errors' "$report"; then
        printf 'Halium check: PASS\n'
    else
        printf 'Halium check: FAIL (see output above)\n'
        missing=$((missing + 1))
    fi
else
    printf 'warning: %s is not available; skipped upstream check\n' "$HALIUM_CHECK" >&2
fi

if ((missing)); then
    printf '\n%d required check(s) failed.\n' "$missing" >&2
    exit 1
fi

printf '\nAll selected checks passed.\n'
