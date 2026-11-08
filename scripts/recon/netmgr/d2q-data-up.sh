#!/bin/sh
# d2q-data-up.sh — re-establish the full RMNET/IPA data path on the d2q after boot.
# Order matches the verified stock sequence. Run on the phone as root.
set -e
cd /root

echo "== 1. IPA fw (one-shot, kernel-guarded) =="
[ -c /dev/ipa ] && echo 1 > /dev/ipa || true
sleep 2
ip link set rmnet_ipa0 up && echo "rmnet_ipa0 set UP"

echo "== 2. rmnet_data1 (create if absent) + setlink =="
ip link show rmnet_data1 >/dev/null 2>&1 || \
  ip link add link rmnet_ipa0 name rmnet_data1 type rmnet mux_id 1
./rmnet-rtm rmnet_data1 rmnet_ipa0 0x4000000D 0x4000000D 16384 32 0 2987712
ip link set rmnet_data1 up

echo "== 3. ADD_MUX_CHANNEL mux 1 =="
./rmnet-setup addmux 1 rmnet_data1

echo "== 4. ioctls: egress 0x0e, ingress 0x2e =="
./rmnet-setup egress 0x0e
./rmnet-setup ingress 0x2e 16384 31

echo "== 5. DPM open port =="
qmicli -d qrtr://0 --dpm-open-port="ctrl-ep-type=embedded,ctrl-ep-iface-number=1,ctrl-port-name=rmnet_ipa0,hw-data-ep-type=embedded,hw-data-ep-iface-number=1,hw-data-rx-id=2,hw-data-tx-id=11" --client-no-release-cid

echo "== 6. WDS bind+start (one process, holds the call) =="
setsid nohup ./wds-call > /root/wds-call.log 2>&1 </dev/null &
sleep 8
cat /root/wds-call.log
