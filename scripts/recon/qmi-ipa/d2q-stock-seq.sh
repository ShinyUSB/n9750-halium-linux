#!/bin/sh
# d2q-stock-seq.sh — full stock-faithful data-call sequence on the d2q.
# Order recovered from stock netmgrd (RMNET-NETMGR-STOCK-ANALYSIS):
#   IPA fw -> VND create -> ADD_MUX -> DPM open -> WDA set-data-format ->
#   RTM flags -> egress/ingress ioctls -> WDS bind+start -> DL rule.
# Requires ns-lite v3 (QMI_IPA handshake) complete at boot.
set -x
cd /root

# 1. IPA fw (one-shot, kernel-guarded)
[ -c /dev/ipa ] && echo 1 > /dev/ipa 2>/dev/null
sleep 1

# 2. lower device UP (rmnet_ipa0 must be UP before children forward)
ip link set rmnet_ipa0 up

# 3. create rmnet_data1 (mux 1)
ip link show rmnet_data1 >/dev/null 2>&1 || \
  ip link add link rmnet_ipa0 name rmnet_data1 type rmnet mux_id 1

# 4. ADD_MUX_CHANNEL
./rmnet-setup addmux 1 rmnet_data1

# 5. DPM open port (data-port gate). rx/tx endpoint ids are parameterized:
#    usage: d2q-stock-seq.sh [rx_id] [tx_id]   (default 2 11 = the prior values)
# Default to the endpoint IDs corrected for the GET_EP_PAIR field swap:
#   kernel fills producer_pipe_num = ep_mapping(APPS_WAN_CONS) [DL consumer],
#                    consumer_pipe_num = ep_mapping(APPS_WAN_PROD) [UL producer].
#   rmnet-inspect printed "consumer=2 producer=11" => UL prod = ep 2, DL cons = ep 11.
#   DPM rx = AP-receives (DL consumer) = 11, tx = AP-transmits (UL producer) = 2.
# Override on the cmdline to A/B:  d2q-stock-seq.sh 2 11  reproduces the old mapping.
RXID=${1:-11}
TXID=${2:-2}
echo "DPM open with rx=$RXID tx=$TXID"
qmicli -d qrtr://0 --dpm-open-port="ctrl-ep-type=embedded,ctrl-ep-iface-number=1,ctrl-port-name=rmnet_ipa0,hw-data-ep-type=embedded,hw-data-ep-iface-number=1,hw-data-rx-id=$RXID,hw-data-tx-id=$TXID" --client-no-release-cid

# 6. WDA SET_DATA_FORMAT (now succeeds post-handshake; granted proto=0=plain QMAP)
python3 /root/wda_raw.py

# 7. RTM port flags (QMAP, no UL-core-agg) + UL agg params
./rmnet-rtm rmnet_data1 rmnet_ipa0 0x4000000D 0x4000000D 16384 32 0 2987712

# 8. ioctls: stock msmnile = QMAPV4 + aggregation both dirs (matches the WDA grant):
#    egress 0x0e (MAP|AGGREGATION|MUXING), ingress 0x2e (MAP|DEAGG|DEMUX|AGG_DATA)
./rmnet-setup egress 0x1e
./rmnet-setup ingress 0x3e 16384 31
ip link set rmnet_data1 up

# 9. WDS bind+start, hold the call
setsid nohup ./wds-call > /root/wds-call.log 2>&1 </dev/null &
sleep 8
grep -E "ipv4 address|ipv4 gateway|===" /root/wds-call.log
CALLIP=$(awk '/ipv4 address:/{print $3}' /root/wds-call.log)
GW=$(awk '/ipv4 gateway:/{print $3}' /root/wds-call.log)
echo "CALLIP=$CALLIP GW=$GW"
ip addr add $CALLIP/32 dev rmnet_data1 2>/dev/null
ip route add $GW/32 dev rmnet_data1 2>/dev/null
ip route add default via $GW dev rmnet_data1 metric 700 2>/dev/null

# 10. DL filter rule to the modem (ipacm step)
./dl-rule $CALLIP 1

# 11. test
dmesg -C 2>/dev/null
python3 - <<PYEOF
import socket, time
dev=b"rmnet_data1\0"
for host,port,tag in [("$GW",53,"MODEM-GW"),("8.8.8.8",53,"8.8.8.8:53"),("1.1.1.1",443,"1.1.1.1:443")]:
    s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_BINDTODEVICE,dev); s.settimeout(6)
    t=time.time()
    try: s.connect((host,port)); print("%s CONNECTED in %.2fs" % (tag,time.time()-t))
    except Exception as e: print("%s FAIL: %s" % (tag,e))
    s.close()
PYEOF
sleep 1
echo "=== RESULTS ==="
echo "rmnet_ipa0 tx/rx: $(cat /sys/class/net/rmnet_ipa0/statistics/tx_packets)/$(cat /sys/class/net/rmnet_ipa0/statistics/rx_packets)"
echo "rmnet_data1 tx/rx: $(cat /sys/class/net/rmnet_data1/statistics/tx_packets)/$(cat /sys/class/net/rmnet_data1/statistics/rx_packets)"
echo "D2QRX:"; dmesg | grep -a D2QRX | head -10
echo "=== STOCK-SEQ DONE ==="
