#!/usr/bin/env bash
# Host-side observer for a d2q bring-up run.
#
# Records only: USB enumeration, link/address state, and the host kernel log.
# It never writes to the phone and never runs a session-consuming Odin command.
# Everything is change-triggered, so a quiet device produces no output and an
# active one produces a timestamped snapshot of exactly what changed.
set -u

OUT=${1:?usage: observe-host.sh <output-file>}
INTERVAL=${INTERVAL:-2}

snapshot() {
    {
        echo "--- usb ---"
        lsusb
        echo "--- links ---"
        ip -br link
        echo "--- addrs ---"
        ip -br addr
        echo "--- routes to 192.168.2.0/24 and 10.15.19.0/24 ---"
        ip route get 192.168.2.15 2>&1 | head -2
        ip route get 10.15.19.82 2>&1 | head -2
    } 2>/dev/null
}

prev=""
while :; do
    cur=$(snapshot)
    if [ "$cur" != "$prev" ]; then
        {
            echo "=== $(date -Is) change ==="
            echo "$cur"
        } >> "$OUT"
        prev="$cur"
    fi
    sleep "$INTERVAL"
done
