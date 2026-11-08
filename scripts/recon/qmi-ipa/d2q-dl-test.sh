#!/bin/sh
# d2q-dl-test.sh — clean end-to-end data-path + DL test on the d2q.
# Runs AFTER a fresh boot with ns-lite v3 (QMI_IPA handshake completes).
# Order follows the verified stock sequence. Captures diagnostics for DL.
set -x
cd /root

# 1. IPA fw (one-shot, kernel-guarded; usually already done by d2q-ipa-fw)
[ -c /dev/ipa ] && echo 1 > /dev/ipa 2>/dev/null
sleep 1

# 2. LOWER DEVICE UP FIRST (the 2026-11-06 bug: rmnet_ipa0 must be UP or the
#    rmnet core cannot forward UL to it)
ip link set rmnet_ipa0 up
ip link show rmnet_ipa0

# 3. rmnet_data1 child + port format (flags 0x4000000D = QMAP, no UL-core-agg)
ip link show rmnet_data1 >/dev/null 2>&1 || \
  ip link add link rmnet_ipa0 name rmnet_data1 type rmnet mux_id 1
./rmnet-rtm rmnet_data1 rmnet_ipa0 0x4000000D 0x4000000D 16384 32 0 2987712
ip link set rmnet_data1 up

# 4. IPA pipe ioctls
./rmnet-setup addmux 1 rmnet_data1
./rmnet-setup egress 0x0e
./rmnet-setup ingress 0x2e 16384 31

# 5. DPM open port (the data-port gate; persists modem-side)
qmicli -d qrtr://0 --dpm-open-port="ctrl-ep-type=embedded,ctrl-ep-iface-number=1,ctrl-port-name=rmnet_ipa0,hw-data-ep-type=embedded,hw-data-ep-iface-number=1,hw-data-rx-id=2,hw-data-tx-id=11" --client-no-release-cid

# 6. WDS bind+start, hold the call
setsid nohup ./wds-call > /root/wds-call.log 2>&1 </dev/null &
sleep 8
grep -E "ipv4 address|ipv4 gateway|packet_data_handle|===" /root/wds-call.log
CALLIP=$(awk '/ipv4 address:/{print $3}' /root/wds-call.log)
GW=$(awk '/ipv4 gateway:/{print $3}' /root/wds-call.log)
echo "CALLIP=$CALLIP GW=$GW"

# 7. IP config on rmnet_data1
ip addr add $CALLIP/32 dev rmnet_data1 2>/dev/null
ip route add $GW/32 dev rmnet_data1 2>/dev/null
ip route add default via $GW dev rmnet_data1 metric 700 2>/dev/null

# 8. DL filter rule to the modem (the ipacm step), matching the call IP
./dl-rule $CALLIP 1
echo "dl-rule rc=$?"

# 9. diagnostics + the decisive test
echo 1 > /sys/kernel/debug/ipa/enable_low_prio_print 2>/dev/null
dmesg -C 2>/dev/null   # clear ring so D2QRX capture is unambiguous
python3 - <<PYEOF
import socket, time
dev=b"rmnet_data1\0"
for host,port,tag in [("$GW",53,"MODEM-GW"),("8.8.8.8",53,"8.8.8.8:53"),("1.1.1.1",443,"1.1.1.1:443")]:
    s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_BINDTODEVICE,dev); s.settimeout(6)
    t=time.time()
    try:
        s.connect((host,port)); print("%s CONNECTED in %.2fs" % (tag,time.time()-t))
    except Exception as e: print("%s FAIL: %s" % (tag,e))
    s.close()
PYEOF
sleep 1
echo "=== RESULTS ==="
echo "rmnet_ipa0 tx/rx: $(cat /sys/class/net/rmnet_ipa0/statistics/tx_packets)/$(cat /sys/class/net/rmnet_ipa0/statistics/rx_packets)"
echo "rmnet_data1 tx/rx: $(cat /sys/class/net/rmnet_data1/statistics/tx_packets)/$(cat /sys/class/net/rmnet_data1/statistics/rx_packets)"
echo "D2QRX (WAN consumer handler) lines:"
dmesg | grep D2QRX | head -15
echo "D2QQMI lines:"
dmesg | grep D2QQMI | head
echo "=== DL-TEST DONE ==="
