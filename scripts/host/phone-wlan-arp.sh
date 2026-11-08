#!/bin/sh
# Keep a static ARP entry for the Debian phone on the 346C WLAN.
#
# Why: the phone's WiFi stack does not answer ARP requests for its own address,
# so the host cannot resolve 192.168.1.16 on its own (neighbour goes FAILED,
# "No route to host"). The phone's ARP *requests* work fine, which is why the
# phone can reach the host while the host cannot reach the phone. With the
# mapping pinned, both directions work.
#
# Run from the HOST after the phone has its WLAN IP (and after re-flushing the
# neighbour cache, since a FAILED entry is sticky).

set -e

PHONE_IP=${PHONE_IP:-192.168.1.16}
PHONE_MAC=${PHONE_MAC:-00:13:e0:fd:41:2a}
HOST_IF=${HOST_IF:-enp3s0}

echo "pinning $PHONE_IP -> $PHONE_MAC on $HOST_IF"
ip neigh flush dev "$HOST_IF" 2>/dev/null || true
ip neigh replace "$PHONE_IP" lladdr "$PHONE_MAC" dev "$HOST_IF" nud permanent
ip neigh show "$PHONE_IP"

echo "verifying..."
ping -c 3 -W 4 "$PHONE_IP" | tail -n 3