#!/bin/sh
# d2q WLAN bring-up: supplicant + addressing + clock sync.
#
# The Broadcom BCM4375 WLAN is driven by the built-in bcmdhd (bcmdhd_101_16)
# driver. Device-tree power/reset of the PCIe endpoint comes from the stock
# Samsung DTBO overlay (bcmdhd_wlan node: wlan-en-gpio 65, host-wake 68), so
# by the time this script runs the PCI endpoint 14e4:4475 is already up and
# firmware has been loaded from /lib/firmware. All this script does is userspace
# association and IP configuration.
#
# Addressing order: try DHCP, fall back to static. The stock OpenWrt access
# point used during bring-up does not answer DHCP, hence the static default.
# Override via environment / systemd drop-in if your network differs.
set -u

IFACE=${D2Q_WLAN_IFACE:-wlan0}
SSID_CFG=${D2Q_WLAN_CONF:-/etc/wpa_supplicant.conf}
STATIC_MAP=${D2Q_WLAN_STATIC_MAP:-/etc/d2q-wlan-static.conf}
DHCP_WAIT=${D2Q_WLAN_DHCP_WAIT:-30}
ASSOC_WAIT=${D2Q_WLAN_ASSOC_WAIT:-45}

log() { echo "d2q-wlan: $*"; }

# --- 1. wait for the WLAN interface -------------------------------------
i=0
while [ ! -e "/sys/class/net/$IFACE" ]; do
    i=$((i + 1))
    [ "$i" -gt 30 ] && { log "$IFACE never appeared"; exit 1; }
    sleep 1
done
ip link set "$IFACE" up

# --- 2. association -----------------------------------------------------
if ! pidof wpa_supplicant >/dev/null 2>&1; then
    wpa_supplicant -B -i "$IFACE" -c "$SSID_CFG" \
        || { log "wpa_supplicant failed to start"; exit 1; }
fi

i=0
while [ "$i" -lt "$ASSOC_WAIT" ]; do
    if wpa_cli -i "$IFACE" status 2>/dev/null | grep -q 'wpa_state=COMPLETED'; then
        log "associated with $(wpa_cli -i "$IFACE" status 2>/dev/null | sed -n 's/^ssid=//p')"
        break
    fi
    i=$((i + 1))
    sleep 1
done
if ! wpa_cli -i "$IFACE" status 2>/dev/null | grep -q 'wpa_state=COMPLETED'; then
    log "association did not complete in ${ASSOC_WAIT}s"
fi

# --- 3. addressing: DHCP first, per-SSID static fallback ----------------
# The static fallback is looked up per SSID so it can never be applied to a
# network it does not belong to. Format of the static map (whitespace
# separated):  <SSID> <addr/prefix> <gateway> [dns]
# Networks that serve DHCP need no entry at all.
SSID=$(wpa_cli -i "$IFACE" status 2>/dev/null | sed -n 's/^ssid=//p')
[ -n "$SSID" ] || SSID="(unknown)"

got_addr=0
if command -v dhclient >/dev/null 2>&1; then
    log "trying DHCP on '$SSID'"
    if timeout "$DHCP_WAIT" dhclient -1 -q \
            -pf "/run/dhclient-$IFACE.pid" -lf "/var/lib/dhcp/$IFACE.leases" "$IFACE" 2>/dev/null; then
        log "got DHCP lease on '$SSID'"
        got_addr=1
    fi
fi

if [ "$got_addr" -eq 0 ]; then
    entry=$(awk -v s="$SSID" '$1 == s { print; exit }' "$STATIC_MAP" 2>/dev/null)
    if [ -n "$entry" ]; then
        # shellcheck disable=SC2086
        set -- $entry
        log "no DHCP offer on '$SSID'; static $2 via $3"
        ip addr replace "$2" dev "$IFACE"
        ip route replace default via "$3" dev "$IFACE"
        if [ -n "${4:-}" ]; then
            printf 'nameserver %s\n' "$4" > /etc/resolv.conf
        fi
    else
        log "no DHCP offer on '$SSID' and no static entry; left unconfigured"
        log "(add one to $STATIC_MAP if that network has no DHCP server)"
    fi
fi

# --- 4. clock (this board has no usable RTC; timesyncd stays masked) ----
if [ -z "${D2Q_WLAN_NO_CLOCK:-}" ] && command -v curl >/dev/null 2>&1; then
    d=$(curl -sI --max-time 10 http://deb.debian.org/ 2>/dev/null \
        | sed -n 's/^[Dd]ate: //p' | tr -d '\r')
    [ -n "$d" ] && date -s "$d" >/dev/null 2>&1 && log "clock set from network"
fi

log "done: $(ip -br addr show "$IFACE" 2>/dev/null | tr -s ' ')"
exit 0
